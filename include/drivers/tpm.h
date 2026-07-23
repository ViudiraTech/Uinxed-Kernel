/*
 *
 *      tpm.h
 *      Trusted Platform Module header
 *
 *      2026/7/23 Ported from Linux TPM subsystem
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_TPM_H_
#define INCLUDE_TPM_H_

#include <drivers/acpi.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* ======================================================================
 *  TPM hardware addresses and sizes
 * ====================================================================== */

#define TPM_LEGACY_BASE_PHYS 0xFED40000
#define TPM_BUFSIZE          4096
#define TPM_HEADER_SIZE      10
#define TPM_RETRY            50

/* ======================================================================
 *  TPM ACPI start method types
 * ====================================================================== */

#define ACPI_TPM2_START_METHOD                     2
#define ACPI_TPM2_COMMAND_BUFFER                   6 /* TIS/FIFO */
#define ACPI_TPM2_COMMAND_BUFFER_WITH_START_METHOD 7 /* CRB with ACPI Start */
#define ACPI_TPM2_MEMORY_MAPPED                    8 /* CRB MMIO */
#define ACPI_TPM2_COMMAND_BUFFER_WITH_ARM_SMC      11
#define ACPI_TPM2_COMMAND_BUFFER_WITH_PLUTON       12
#define ACPI_TPM2_CRB_WITH_ARM_FFA                 13

/* ======================================================================
 *  TPM version and interface type
 * ====================================================================== */

typedef enum {
    TPM_VERSION_NONE = 0,
    TPM_VERSION_12   = 1,
    TPM_VERSION_20   = 2,
} tpm_version_t;

typedef enum {
    TPM_IFACE_NONE = 0,
    TPM_IFACE_TIS  = 1,
    TPM_IFACE_CRB  = 2,
} tpm_interface_t;

/* ======================================================================
 *  TPM flags
 * ====================================================================== */

#define TPM_FLAG_TPM2 (1 << 0)
#define TPM_FLAG_TIS  (1 << 1)
#define TPM_FLAG_CRB  (1 << 2)

/* ======================================================================
 *  TPM vendor IDs (DID_VID register: DID << 16 | VID)
 * ====================================================================== */

#define TPM_VID_ATML    0x1114
#define TPM_VID_INTEL   0x8086
#define TPM_VID_STM     0x104A
#define TPM_VID_WINBOND 0x1050
#define TPM_VID_IFX     0x15D1

/* ======================================================================
 *  TPM error/warning codes
 * ====================================================================== */

#define TPM_WARN_RETRY          0x800
#define TPM_WARN_DOING_SELFTEST 0x802
#define TPM_ERR_DEACTIVATED     0x6
#define TPM_ERR_DISABLED        0x7
#define TPM_ERR_FAILEDSELFTEST  0x1C

/* ======================================================================
 *  TIS register offsets (locality is shifted left by 12)
 * ====================================================================== */

#define TIS_REG_ACCESS(l)     (0x0000 | ((l) << 12))
#define TIS_REG_INT_ENABLE(l) (0x0008 | ((l) << 12))
#define TIS_REG_INT_VECTOR(l) (0x000C | ((l) << 12))
#define TIS_REG_INT_STATUS(l) (0x0010 | ((l) << 12))
#define TIS_REG_INTF_CAPS(l)  (0x0014 | ((l) << 12))
#define TIS_REG_STS(l)        (0x0018 | ((l) << 12))
#define TIS_REG_DATA_FIFO(l)  (0x0024 | ((l) << 12))
#define TIS_REG_DID_VID(l)    (0x0F00 | ((l) << 12))
#define TIS_REG_RID(l)        (0x0F04 | ((l) << 12))

/* TIS memory length */
#define TIS_MEM_LEN 0x5000

/* TIS status register bits */
#define TPM_STS_VALID          0x80
#define TPM_STS_COMMAND_READY  0x40
#define TPM_STS_GO             0x20
#define TPM_STS_DATA_AVAIL     0x10
#define TPM_STS_DATA_EXPECT    0x08
#define TPM_STS_SELFTEST_DONE  0x04
#define TPM_STS_RESPONSE_RETRY 0x02
#define TPM_STS_READ_ZERO      0x23 /* bits must be zero on valid read */

/* TIS access register bits */
#define TPM_ACCESS_VALID           0x80
#define TPM_ACCESS_ACTIVE_LOCALITY 0x20
#define TPM_ACCESS_REQUEST_PENDING 0x04
#define TPM_ACCESS_REQUEST_USE     0x02

/* TIS interrupt enable bits */
#define TPM_GLOBAL_INT_ENABLE        0x80000000
#define TPM_INTF_BURST_COUNT_STATIC  0x100
#define TPM_INTF_CMD_READY_INT       0x080
#define TPM_INTF_INT_EDGE_FALLING    0x040
#define TPM_INTF_INT_EDGE_RISING     0x020
#define TPM_INTF_INT_LEVEL_LOW       0x010
#define TPM_INTF_INT_LEVEL_HIGH      0x008
#define TPM_INTF_LOCALITY_CHANGE_INT 0x004
#define TPM_INTF_STS_VALID_INT       0x002
#define TPM_INTF_DATA_AVAIL_INT      0x001

/* TIS timeout defaults (milliseconds) */
#define TIS_SHORT_TIMEOUT 750
#define TIS_LONG_TIMEOUT  2000
#define TPM_TIMEOUT_A     750
#define TPM_TIMEOUT_B     2000
#define TPM_TIMEOUT_C     750
#define TPM_TIMEOUT_D     750

/* ======================================================================
 *  CRB register offsets
 * ====================================================================== */

#define CRB_LOC_STATE_OFFSET   0x000
#define CRB_LOC_CTRL_OFFSET    0x008
#define CRB_LOC_STS_OFFSET     0x00C
#define CRB_CTRL_REQ_OFFSET    0x040
#define CRB_CTRL_STS_OFFSET    0x044
#define CRB_CTRL_CANCEL_OFFSET 0x048
#define CRB_CTRL_START_OFFSET  0x04C
#define CRB_CTRL_CMD_SIZE      0x058
#define CRB_CTRL_CMD_PA_L      0x05C
#define CRB_CTRL_CMD_PA_H      0x060
#define CRB_CTRL_RSP_SIZE      0x064
#define CRB_CTRL_RSP_PA_L      0x068
#define CRB_CTRL_RSP_PA_H      0x06C

/* CRB register bit definitions */
#define CRB_LOC_CTRL_REQUEST_ACCESS (1 << 0)
#define CRB_LOC_CTRL_RELINQUISH     (1 << 1)
#define CRB_LOC_STATE_LOC_ASSIGNED  (1 << 1)
#define CRB_LOC_STATE_TPM_REG_VALID (1 << 7)
#define CRB_CTRL_REQ_CMD_READY      (1 << 0)
#define CRB_CTRL_REQ_GO_IDLE        (1 << 1)
#define CRB_CTRL_STS_ERROR          (1 << 0)
#define CRB_CTRL_STS_TPM_IDLE       (1 << 1)
#define CRB_START_INVOKE            (1 << 0)
#define CRB_CANCEL_INVOKE           (1 << 0)
#define CRB_DRV_STS_COMPLETE        (1 << 0)

/* ======================================================================
 *  TPM command tags
 * ====================================================================== */

#define TPM_TAG_RQU_COMMAND 0x00C1
#define TPM2_ST_NO_SESSIONS 0x8001
#define TPM2_ST_SESSIONS    0x8002

/* ======================================================================
 *  TPM 1.2 command ordinals
 * ====================================================================== */

#define TPM_ORD_GET_CAPABILITY    0x00000065
#define TPM_ORD_GET_RANDOM        0x00000046
#define TPM_ORD_PCR_READ          0x00000015
#define TPM_ORD_PCR_EXTEND        0x00000014
#define TPM_ORD_SELF_TEST_FULL    0x00000050
#define TPM_ORD_CONTINUE_SELFTEST 0x00000053
#define TPM_ORD_GET_TICKS         0x000000F1
#define TPM_ORD_STARTUP           0x00000099

/* ======================================================================
 *  TPM 2.0 command codes
 * ====================================================================== */

#define TPM2_CC_STARTUP         0x00000144
#define TPM2_CC_SELF_TEST       0x00000143
#define TPM2_CC_GET_CAPABILITY  0x0000017A
#define TPM2_CC_GET_RANDOM      0x0000017B
#define TPM2_CC_PCR_READ        0x0000017E
#define TPM2_CC_PCR_EXTEND      0x00000182
#define TPM2_CC_SHUTDOWN        0x00000145
#define TPM2_CC_GET_TEST_RESULT 0x0000017C

/* TPM 2.0 startup types */
#define TPM2_SU_CLEAR 0x0000
#define TPM2_SU_STATE 0x0001

/* ======================================================================
 *  TPM 2.0 property IDs (for GetCapability)
 * ====================================================================== */

#define TPM2_PT_FAMILY_INDICATOR   0x00000100
#define TPM2_PT_LEVEL              0x00000101
#define TPM2_PT_REVISION           0x00000102
#define TPM2_PT_MANUFACTURER       0x00000105
#define TPM2_PT_VENDOR_STRING_1    0x00000106
#define TPM2_PT_VENDOR_STRING_2    0x00000107
#define TPM2_PT_VENDOR_STRING_3    0x00000108
#define TPM2_PT_VENDOR_STRING_4    0x00000109
#define TPM2_PT_FIRMWARE_VERSION_1 0x0000010B
#define TPM2_PT_FIRMWARE_VERSION_2 0x0000010C
#define TPM2_PT_MAX_COMMAND_SIZE   0x0000011E
#define TPM2_PT_MAX_RESPONSE_SIZE  0x0000011F
#define TPM2_PT_PCR_COUNT          0x00000112
#define TPM2_PT_MANUFACTURER_2     TPM2_PT_MANUFACTURER

/* TPM 2.0 capability types */
#define TPM2_CAP_TPM_PROPERTIES 0x00000006

/* ======================================================================
 *  TPM 2.0 ACPI table structure
 * ====================================================================== */

typedef struct {
        acpi_sdt_header_t header;
        uint16_t          platform_class;
        uint16_t          reserved;
        uint64_t          control_area_address;
        uint32_t          start_method;
} __attribute__((packed)) tpm2_table_t;

typedef struct {
        acpi_sdt_header_t header;
        uint16_t          platform_class;
        uint32_t          log_area_minimum_length;
        uint64_t          log_area_start_address;
} __attribute__((packed)) tcpa_table_t;

/* ======================================================================
 *  TPM device structure
 * ====================================================================== */

typedef struct tpm_device tpm_device_t;

typedef uint8_t (*tpm_status_fn)(tpm_device_t *dev);
typedef int (*tpm_send_fn)(tpm_device_t *dev, uint8_t *buf, size_t len);
typedef int (*tpm_recv_fn)(tpm_device_t *dev, uint8_t *buf, size_t maxlen);
typedef int (*tpm_request_locality_fn)(tpm_device_t *dev, int locality);
typedef void (*tpm_relinquish_locality_fn)(tpm_device_t *dev, int locality);
typedef void (*tpm_cancel_fn)(tpm_device_t *dev);
typedef int (*tpm_ready_fn)(tpm_device_t *dev);

struct tpm_device {
        tpm_version_t   version;   /* TPM version (1.2 or 2.0) */
        tpm_interface_t iface;     /* Interface type (TIS or CRB) */
        void           *mmio_base; /* MMIO base virtual address */
        int             locality;  /* Current locality (0-4) */
        uint32_t        timeout_a; /* Timeout A in ms */
        uint32_t        timeout_b; /* Timeout B in ms */
        uint32_t        timeout_c; /* Timeout C in ms */
        uint32_t        timeout_d; /* Timeout D in ms */
        uint32_t        did_vid;   /* Device ID << 16 | Vendor ID */
        uint8_t         rid;       /* Revision ID */
        uint32_t        flags;     /* TPM_FLAG_* */

        /* Interface operations */
        tpm_status_fn              status;
        tpm_send_fn                send;
        tpm_recv_fn                recv;
        tpm_request_locality_fn    request_locality;
        tpm_relinquish_locality_fn relinquish_locality;
        tpm_cancel_fn              cancel;
        tpm_ready_fn               ready;

        /* CRB-specific */
        void    *crb_cmd_buf;  /* CRB command buffer */
        void    *crb_rsp_buf;  /* CRB response buffer */
        uint32_t crb_cmd_size; /* CRB command buffer size */
        uint32_t crb_sm;       /* CRB start method */
};

/* ======================================================================
 *  TPM public API
 * ====================================================================== */

/* Initialize TPM subsystem (auto-detection, interface init, startup) */
int tpm_init(void);

/* Get the global TPM device (NULL if none found) */
tpm_device_t *tpm_get_device(void);

/* Transmit a command buffer and receive response in-place.
 * On input, buf contains the command; on output it contains the response.
 * Returns response length on success, negative on error. */
int tpm_transmit(tpm_device_t *dev, uint8_t *buf, size_t bufsiz, size_t len);

/* High-level TPM 2.0 commands */

/* Get a TPM 2.0 property value (32-bit). Returns 0 on success. */
int tpm2_get_property(tpm_device_t *dev, uint32_t property, uint32_t *value);

/* Get random bytes from TPM. Returns number of bytes read, negative on error. */
int tpm_get_random(tpm_device_t *dev, uint8_t *out, size_t max);

/* Read a PCR value. digest must be at least 32 bytes for SHA-256.
 * Returns digest size on success, negative on error. */
int tpm2_pcr_read(tpm_device_t *dev, uint32_t pcr_idx, uint8_t *digest);

/* TPM 1.2 - get random bytes */
int tpm1_get_random(tpm_device_t *dev, uint8_t *out, size_t max);

/* ======================================================================
 *  Internal helper utilities shared between TIS/CRB modules
 * ====================================================================== */

/* Busy-wait delay in microseconds (crude, for TPM init timeouts) */
void tpm_udelay(uint32_t us);

/* Spin-loop timeout helper: calls check(ctx) until true or timeout_ms */
int tpm_poll_timeout(int (*check)(void *ctx), void *ctx, uint32_t timeout_ms);

/* TIS module APIs (called from tpm.c) */
int tpm_tis_init(tpm_device_t *dev);

/* CRB module APIs (called from tpm.c) */
int tpm_crb_init(tpm_device_t *dev);

#endif /* INCLUDE_TPM_H_ */
