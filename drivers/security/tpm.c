/*
 *
 *      tpm.c
 *      Trusted Platform Module - Core driver
 *
 *      2026/7/23 By MicroFish
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/acpi.h>
#include <drivers/tpm.h>
#include <kernel/printk.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/hhdm.h>

static tpm_device_t g_tpm_device;
static int          g_tpm_available = 0;

tpm_device_t *tpm_get_device(void)
{
    return g_tpm_available ? &g_tpm_device : NULL;
}

/* ======================================================================
 *  Timeout and delay helpers (HPET-based for accuracy)
 * ====================================================================== */

void tpm_udelay(uint32_t us)
{
    uint64_t target = nano_time() + (uint64_t)us * 1000ULL;
    while (nano_time() < target) { __asm__ volatile("pause" ::: "memory"); }
}

int tpm_poll_timeout(int (*check)(void *ctx), void *ctx, uint32_t timeout_ms)
{
    uint64_t deadline = nano_time() + (uint64_t)timeout_ms * 1000000ULL;
    int      rc;

    for (;;) {
        rc = check(ctx);
        if (rc != 0) return rc;
        if (nano_time() >= deadline) return 0;
        tpm_udelay(100);
    }
}

/* ======================================================================
 *  Endianness helpers (inlined, no dependency on external libraries)
 * ====================================================================== */

static uint16_t be16_to_cpu(uint16_t x)
{
    uint8_t *b = (uint8_t *)&x;
    return ((uint16_t)b[0] << 8) | b[1];
}

static uint32_t be32_to_cpu(uint32_t x)
{
    uint8_t *b = (uint8_t *)&x;
    return ((uint32_t)b[0] << 24) | ((uint32_t)b[1] << 16) | ((uint32_t)b[2] << 8) | b[3];
}

static void cpu_to_be16(uint16_t x, uint8_t out[2])
{
    out[0] = (x >> 8) & 0xFF;
    out[1] = x & 0xFF;
}

static void cpu_to_be32(uint32_t x, uint8_t out[4])
{
    out[0] = (x >> 24) & 0xFF;
    out[1] = (x >> 16) & 0xFF;
    out[2] = (x >> 8) & 0xFF;
    out[3] = x & 0xFF;
}

/* ======================================================================
 *  Common transmit function
 * ====================================================================== */

int tpm_transmit(tpm_device_t *dev, uint8_t *buf, size_t bufsiz, size_t len)
{
    int rc;

    if (!dev || !buf) return -1;
    for (uint32_t retry = 0; retry < TPM_RETRY; retry++) {
        rc = dev->request_locality(dev, 0);
        if (rc < 0) {
            dev->relinquish_locality(dev, 0);
            tpm_udelay(500);
            continue;
        }

        rc = dev->send(dev, buf, len);
        if (rc < 0) {
            dev->relinquish_locality(dev, 0);
            tpm_udelay(1000);
            continue;
        }

        rc = dev->recv(dev, buf, bufsiz);
        dev->relinquish_locality(dev, 0);

        if (rc < 0) {
            tpm_udelay(1000);
            continue;
        }
        return rc;
    }
    return -1;
}

/* ======================================================================
 *  TPM 2.0: build and send a command (no sessions)
 * ====================================================================== */

static int tpm2_send_command(tpm_device_t *dev, uint32_t cc, const uint8_t *params, uint32_t param_len, uint8_t *rsp_buf, size_t rsp_buf_size)
{
    uint8_t cmd_buf[TPM_BUFSIZE];
    if (param_len + 10 > TPM_BUFSIZE) return -1;

    cpu_to_be16(TPM2_ST_NO_SESSIONS, &cmd_buf[0]);
    cpu_to_be32(10 + param_len, &cmd_buf[2]);
    cpu_to_be32(cc, &cmd_buf[6]);
    if (params && param_len > 0) { memcpy(&cmd_buf[10], params, param_len); }

    int rc = tpm_transmit(dev, cmd_buf, sizeof(cmd_buf), 10 + param_len);
    if (rc < 0) return rc;
    if (rc < TPM_HEADER_SIZE) return -1;

    uint32_t rsp_code = be32_to_cpu(*(uint32_t *)&cmd_buf[6]);
    /* TPM warnings (bit 10: TPM_RETRY, bit 11: TPM_DOING_SELFTEST etc.)
     * are not hard errors; the response data may still be valid. */
    if (rsp_code != 0 && !(rsp_code & 0x800)) return -(int)rsp_code;

    uint32_t data_len = (uint32_t)rc - 10;
    if (data_len > rsp_buf_size) data_len = (uint32_t)rsp_buf_size;
    if (rsp_buf && data_len > 0) memcpy(rsp_buf, &cmd_buf[10], data_len);
    return (int)data_len;
}

/* ======================================================================
 *  TPM 2.0 Startup
 * ====================================================================== */

static int tpm2_startup(tpm_device_t *dev)
{
    uint8_t params[2];
    cpu_to_be16(TPM2_SU_CLEAR, params);
    return tpm2_send_command(dev, TPM2_CC_STARTUP, params, 2, NULL, 0);
}

/* ======================================================================
 *  TPM 2.0 GetCapability (property read)
 * ====================================================================== */

int tpm2_get_property(tpm_device_t *dev, uint32_t property, uint32_t *value)
{
    uint8_t params[12];
    uint8_t rsp[32];

    cpu_to_be32(TPM2_CAP_TPM_PROPERTIES, &params[0]);
    cpu_to_be32(property, &params[4]);
    cpu_to_be32(1, &params[8]);

    int rc = tpm2_send_command(dev, TPM2_CC_GET_CAPABILITY, params, 12, rsp, sizeof(rsp));
    if (rc < 0) return rc;

    /* Response body layout (packed, no padding):
     * [0]     moreData     (BYTE, 1)
     * [1..4]  capability   (UINT32, 4)
     * [5..8]  count        (UINT32, 4)
     * [9..12] property[0]  (UINT32, 4)
     * [13..16] value[0]    (UINT32, 4)     */
    if (rc >= 17 && value) {
        *value = be32_to_cpu(*(uint32_t *)&rsp[13]);
        return 0;
    }
    return -1;
}

/* ======================================================================
 *  TPM GetRandom (v2.0 + v1.2)
 * ====================================================================== */

int tpm_get_random(tpm_device_t *dev, uint8_t *out, size_t max)
{
    if (!dev || !out || max == 0) return -1;
    if (dev->version == TPM_VERSION_12) return tpm1_get_random(dev, out, max);

    uint8_t params[2];
    uint8_t rsp[128];

    if (max > 64) max = 64;
    cpu_to_be16((uint16_t)max, params);

    int rc = tpm2_send_command(dev, TPM2_CC_GET_RANDOM, params, 2, rsp, sizeof(rsp));
    if (rc < 0) return rc;
    if (rc >= 2) {
        uint16_t size = be16_to_cpu(*(uint16_t *)rsp);
        if (size > max) size = (uint16_t)max;
        if (size > 0) memcpy(out, rsp + 2, size);
        return (int)size;
    }
    return -1;
}

/* ======================================================================
 *  TPM 2.0 PCR Read
 * ====================================================================== */

int tpm2_pcr_read(tpm_device_t *dev, uint32_t pcr_idx, uint8_t *digest)
{
    uint8_t params[6];
    uint8_t rsp[64];

    /* PCR selection: 1 bank (TPM_ALG_SHA256 = 0x000B), 3-byte bitmap */
    uint8_t pcr_select[3] = {0, 0, 0};
    if (pcr_idx < 8)
        pcr_select[0] = 1 << pcr_idx;
    else if (pcr_idx < 16)
        pcr_select[1] = 1 << (pcr_idx - 8);
    else if (pcr_idx < 24)
        pcr_select[2] = 1 << (pcr_idx - 16);

    cpu_to_be16(0x000B, &params[0]); /* TPM_ALG_SHA256 */
    params[2] = 3;
    params[3] = pcr_select[0];
    params[4] = pcr_select[1];
    params[5] = pcr_select[2];

    int rc = tpm2_send_command(dev, TPM2_CC_PCR_READ, params, 6, rsp, sizeof(rsp));
    if (rc < 0) return rc;

    /* Response body: updateCounter(4) + pcrSelection(10) + pcrValuesTL(4) + digestSize(2) + digest
     * Offsets in rsp[]: 0=updateCounter, 4=selCnt, 8=hash, 10=selSize, 11=select[3],
     *                   14=digestCnt, 18=digestSize, 20=digest data */
    if (rc >= 21) {
        uint16_t digest_size = be16_to_cpu(*(uint16_t *)&rsp[18]);
        if (digest_size > 32) digest_size = 32;
        if (digest && digest_size > 0) memcpy(digest, &rsp[20], digest_size);
        return (int)digest_size;
    }
    return -1;
}

/* ======================================================================
 *  TPM 1.2 GetRandom
 * ====================================================================== */

int tpm1_get_random(tpm_device_t *dev, uint8_t *out, size_t max)
{
    uint8_t cmd_buf[TPM_BUFSIZE];
    if (max > 128) max = 128;

    cpu_to_be16(TPM_TAG_RQU_COMMAND, &cmd_buf[0]);
    cpu_to_be32(14, &cmd_buf[2]);
    cpu_to_be32(TPM_ORD_GET_RANDOM, &cmd_buf[6]);
    cpu_to_be32((uint32_t)max, &cmd_buf[10]);

    int rc = tpm_transmit(dev, cmd_buf, sizeof(cmd_buf), 14);
    if (rc < 0) return rc;
    if (rc < TPM_HEADER_SIZE) return -1;

    uint32_t rsp_code = be32_to_cpu(*(uint32_t *)&cmd_buf[6]);
    if (rsp_code != 0) return -(int)rsp_code;
    if (rc >= 14) {
        uint32_t count = be32_to_cpu(*(uint32_t *)&cmd_buf[10]);
        if (count > max) count = (uint32_t)max;
        if (count > 0) memcpy(out, &cmd_buf[14], count);
        return (int)count;
    }
    return -1;
}

/* ======================================================================
 *  Log TPM capabilities after successful init
 * ====================================================================== */

static void tpm_log_capabilities(tpm_device_t *dev)
{
    uint32_t value;
    if (dev->version == TPM_VERSION_20) {
        if (tpm2_get_property(dev, TPM2_PT_MANUFACTURER, &value) == 0) plogk("tpm: Manufacturer ID: 0x%08x\n", value);
        if (tpm2_get_property(dev, TPM2_PT_FIRMWARE_VERSION_1, &value) == 0) plogk("tpm: Firmware version: 0x%08x\n", value);
        if (tpm2_get_property(dev, TPM2_PT_PCR_COUNT, &value) == 0) plogk("tpm: PCR count: %u\n", value);
        if (tpm2_get_property(dev, TPM2_PT_MAX_COMMAND_SIZE, &value) == 0) plogk("tpm: Max command size: %u\n", value);
        if (tpm2_get_property(dev, TPM2_PT_MAX_RESPONSE_SIZE, &value) == 0) plogk("tpm: Max response size: %u\n", value);
        if (tpm2_get_property(dev, TPM2_PT_REVISION, &value) == 0) plogk("tpm: Revision: 0x%08x\n", value);
    }
}

/* ======================================================================
 *  Hardware probe: read DID/VID from MMIO at legacy address
 * ====================================================================== */

static uint32_t tpm_verify_mmio(void *virt_addr)
{
    volatile uint32_t *ptr = (volatile uint32_t *)((uintptr_t)virt_addr + TIS_REG_DID_VID(0));
    uint32_t           val = *ptr;
    if (val == 0x00000000 || val == 0xFFFFFFFF) return 0;
    return val;
}

/* ======================================================================
 *  Main TPM initialization
 * ====================================================================== */

int tpm_init(void)
{
    memset(&g_tpm_device, 0, sizeof(g_tpm_device));
    g_tpm_device.locality = -1;

    tpm2_table_t *tpm2 = (tpm2_table_t *)find_table("TPM2");
    tcpa_table_t *tcpa = (tcpa_table_t *)find_table("TCPA");

    /* --- TPM 2.0 via TPM2 ACPI table --- */
    if (tpm2) {
        plogk("tpm: Found TPM2 ACPI table at %p\n", tpm2);
        uint32_t method    = tpm2->start_method;
        uint64_t ctrl_addr = tpm2->control_area_address;
        plogk("tpm: Start method: 0x%x, Control area: 0x%llx\n", method, ctrl_addr);

        g_tpm_device.version = TPM_VERSION_20;
        g_tpm_device.flags |= TPM_FLAG_TPM2;

        switch (method) {
            case ACPI_TPM2_COMMAND_BUFFER :
                plogk("tpm: Interface: TIS (FIFO)\n");
                g_tpm_device.iface = TPM_IFACE_TIS;
                g_tpm_device.flags |= TPM_FLAG_TIS;
                break;
            case ACPI_TPM2_START_METHOD :
            case ACPI_TPM2_COMMAND_BUFFER_WITH_START_METHOD :
            case ACPI_TPM2_MEMORY_MAPPED :
                plogk("tpm: Interface: CRB (method 0x%x)\n", method);
                g_tpm_device.iface = TPM_IFACE_CRB;
                g_tpm_device.flags |= TPM_FLAG_CRB;
                g_tpm_device.crb_sm = method;
                break;
            default :
                plogk("tpm: Unsupported start method 0x%x, probing legacy TIS.\n", method);
                g_tpm_device.iface = TPM_IFACE_TIS;
                g_tpm_device.flags |= TPM_FLAG_TIS;
                break;
        }

        /* For TIS, try ACPI address first; if invalid or unsupported method, probe legacy */
        if (g_tpm_device.iface == TPM_IFACE_TIS) {
            int use_legacy = 0;
            if (ctrl_addr == 0 || ctrl_addr < 0x1000) {
                plogk("tpm: Invalid control area, probing legacy 0x%lx\n", TPM_LEGACY_BASE_PHYS);
                use_legacy = 1;
            } else {
                /* Try ACPI-provided address first */
                g_tpm_device.mmio_base = phys_to_virt(ctrl_addr);
                uint32_t probe         = tpm_verify_mmio(g_tpm_device.mmio_base);
                if (!probe) {
                    plogk("tpm: No TPM at control area, probing legacy 0x%lx\n", TPM_LEGACY_BASE_PHYS);
                    use_legacy = 1;
                }
            }
            if (use_legacy) {
                ctrl_addr              = TPM_LEGACY_BASE_PHYS;
                g_tpm_device.mmio_base = phys_to_virt(ctrl_addr);
            }
        } else {
            /* CRB: control_area_address points to tail registers (offset 0x40);
             * adjust mmio_base to head register base. */
            uint64_t head          = (ctrl_addr >= 0x40) ? ctrl_addr - 0x40 : ctrl_addr;
            g_tpm_device.mmio_base = phys_to_virt(head);
        }
        g_tpm_device.timeout_a = TPM_TIMEOUT_A;
        g_tpm_device.timeout_b = TPM_TIMEOUT_B;
        g_tpm_device.timeout_c = TPM_TIMEOUT_C;
        g_tpm_device.timeout_d = TPM_TIMEOUT_D;

        int rc = (g_tpm_device.iface == TPM_IFACE_TIS) ? tpm_tis_init(&g_tpm_device) : tpm_crb_init(&g_tpm_device);
        if (rc < 0) {
            plogk("tpm: Interface init failed: %d\n", rc);
            return rc;
        }
        plogk("tpm: TPM 2.0 detected (DID/VID 0x%08x, RID 0x%02x)\n", g_tpm_device.did_vid, g_tpm_device.rid);

        rc = tpm2_startup(&g_tpm_device);
        if (rc < 0 && rc != -TPM_WARN_RETRY) plogk("tpm: Startup returned 0x%x, TPM may already be started.\n", -rc);

        tpm_log_capabilities(&g_tpm_device);
        g_tpm_available = 1;
        return 0;
    }

    /* --- TPM 1.2 via TCPA ACPI table --- */
    if (tcpa) {
        plogk("tpm: Found TCPA ACPI table at %p\n", tcpa);
        plogk("tpm: Interface: TIS (TPM 1.2)\n");

        g_tpm_device.version = TPM_VERSION_12;
        g_tpm_device.iface   = TPM_IFACE_TIS;
        g_tpm_device.flags |= TPM_FLAG_TIS;
        g_tpm_device.mmio_base = phys_to_virt(TPM_LEGACY_BASE_PHYS);

        uint32_t did_vid = tpm_verify_mmio(g_tpm_device.mmio_base);
        if (!did_vid) return -1;

        g_tpm_device.did_vid   = did_vid;
        g_tpm_device.timeout_a = TPM_TIMEOUT_A;
        g_tpm_device.timeout_b = TPM_TIMEOUT_B;
        g_tpm_device.timeout_c = TPM_TIMEOUT_C;
        g_tpm_device.timeout_d = TPM_TIMEOUT_D;

        int rc = tpm_tis_init(&g_tpm_device);
        if (rc < 0) {
            plogk("tpm: TIS init failed: %d\n", rc);
            return rc;
        }

        plogk("tpm: TPM 1.2 detected (DID/VID 0x%08x, RID 0x%02x)\n", g_tpm_device.did_vid, g_tpm_device.rid);
        g_tpm_available = 1;
        return 0;
    }

    /* --- Fallback: legacy hardware probe --- */
    void    *legacy_virt = phys_to_virt(TPM_LEGACY_BASE_PHYS);
    uint32_t did_vid     = tpm_verify_mmio(legacy_virt);

    if (did_vid) {
        plogk("tpm: Found TPM 1.2 at legacy address (DID/VID: 0x%08x)\n", did_vid);

        g_tpm_device.version = TPM_VERSION_12;
        g_tpm_device.iface   = TPM_IFACE_TIS;
        g_tpm_device.flags |= TPM_FLAG_TIS;
        g_tpm_device.mmio_base = legacy_virt;
        g_tpm_device.did_vid   = did_vid;
        g_tpm_device.timeout_a = TPM_TIMEOUT_A;
        g_tpm_device.timeout_b = TPM_TIMEOUT_B;
        g_tpm_device.timeout_c = TPM_TIMEOUT_C;
        g_tpm_device.timeout_d = TPM_TIMEOUT_D;

        int rc = tpm_tis_init(&g_tpm_device);
        if (rc < 0) {
            plogk("tpm: TIS init failed: %d\n", rc);
            return rc;
        }

        plogk("tpm: TPM 1.2 configured (RID 0x%02x)\n", g_tpm_device.rid);
        g_tpm_available = 1;
        return 0;
    }

    return -1;
}
