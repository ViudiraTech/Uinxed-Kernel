/*
 *
 *      nvme.h
 *      NVMe 1.4 block device driver header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_NVME_H_
#define INCLUDE_DRIVERS_NVME_H_

#include <drivers/pci.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

struct blockdev_device;

/* ================================================================
 *  NVMe Controller Register Map (BAR0 offsets)
 * ================================================================ */

#define NVME_REG_CAP   0x00   /* Controller Capabilities         */
#define NVME_REG_VS    0x08   /* Version                         */
#define NVME_REG_INTMS 0x0C   /* Interrupt Mask Set              */
#define NVME_REG_INTMC 0x10   /* Interrupt Mask Clear            */
#define NVME_REG_CC    0x14   /* Controller Configuration        */
#define NVME_REG_CSTS  0x1C   /* Controller Status               */
#define NVME_REG_NSSR  0x20   /* NVM Subsystem Reset             */
#define NVME_REG_AQA   0x24   /* Admin Queue Attributes          */
#define NVME_REG_ASQ   0x28   /* Admin SQ Base Address (64-bit)  */
#define NVME_REG_ACQ   0x30   /* Admin CQ Base Address (64-bit)  */
#define NVME_REG_DBS   0x1000 /* Doorbell Stride start           */

/* ---- CAP bits ---- */
#define NVME_CAP_MQES(cap)  ((uint32_t)((cap) & 0xFFFF))
#define NVME_CAP_DSTRD(cap) (((cap) >> 32) & 0xF)
#define NVME_CAP_TO(cap)    (((cap) >> 56) & 0xFF)

/* ---- CC bits ---- */
#define NVME_CC_EN           (1ULL << 0)
#define NVME_CC_IOSQES_SHIFT 16
#define NVME_CC_IOCQES_SHIFT 20

/* ---- CSTS bits ---- */
#define NVME_CSTS_RDY (1ULL << 0)
#define NVME_CSTS_CFS (1ULL << 1)

/* ---- Queue entry size exponents (for CC register) ---- */
#define NVME_SQE_SIZE  64
#define NVME_CQE_SIZE  16
#define NVME_SQE_SHIFT 6 /* log2(64)  */
#define NVME_CQE_SHIFT 4 /* log2(16)  */

/* ---- Queue depths ---- */
#define NVME_ADMIN_QSIZE 64
#define NVME_IO_QSIZE    256

/* ---- Limits ---- */
#define NVME_MAX_CONTROLLERS 8
#define NVME_MAX_NAMESPACES  16
#define NVME_SECTOR_SIZE     512

/* ================================================================
 *  NVMe Command Opcodes
 * ================================================================ */

/* Admin commands */
#define NVME_ADMIN_DELETE_IO_SQ 0x00
#define NVME_ADMIN_CREATE_IO_SQ 0x01
#define NVME_ADMIN_DELETE_IO_CQ 0x04
#define NVME_ADMIN_CREATE_IO_CQ 0x05
#define NVME_ADMIN_IDENTIFY     0x06
#define NVME_ADMIN_SET_FEATURES 0x09
#define NVME_ADMIN_GET_FEATURES 0x0A
#define NVME_ADMIN_ASYNC_EVENT  0x0C

/* NVM I/O commands */
#define NVME_NVM_FLUSH 0x00
#define NVME_NVM_WRITE 0x01
#define NVME_NVM_READ  0x02

/* ---- Identify CNS values ---- */
#define NVME_CNS_NAMESPACE  0x00
#define NVME_CNS_CONTROLLER 0x01
#define NVME_CNS_NS_LIST    0x02

/* ---- Feature IDs ---- */
#define NVME_FID_NUM_QUEUES     0x07
#define NVME_FID_IRQ_COALESCING 0x08
#define NVME_FID_IRQ_CONFIG     0x09

/* ---- CQE Status Field Type (SFT) ---- */
#define NVME_SCT_GENERIC          0x0
#define NVME_SCT_COMMAND_SPECIFIC 0x1
#define NVME_SCT_MEDIA_ERROR      0x2
#define NVME_SCT_VENDOR_SPECIFIC  0x7

/* ---- Generic Status Codes ---- */
#define NVME_SC_SUCCESS              0x00
#define NVME_SC_INVALID_OPCODE       0x01
#define NVME_SC_INVALID_FIELD        0x02
#define NVME_SC_CMDID_CONFLICT       0x03
#define NVME_SC_DATA_XFER_ERROR      0x04
#define NVME_SC_ABORTED_PWR_LOSS     0x05
#define NVME_SC_INTERNAL_ERROR       0x06
#define NVME_SC_ABORT_BY_REQUEST     0x07
#define NVME_SC_ABORT_SQ_DELETE      0x08
#define NVME_SC_ABORT_FAILED_FUSE    0x09
#define NVME_SC_ABORT_MISSING_FUSE   0x0A
#define NVME_SC_INVALID_NS           0x0B
#define NVME_SC_CMD_SEQ_ERROR        0x0C
#define NVME_SC_INVALID_SGL_SEG      0x0D
#define NVME_SC_INVALID_NUM_SGL      0x0E
#define NVME_SC_LBA_RANGE            0x80
#define NVME_SC_CAP_EXCEEDED         0x81
#define NVME_SC_NS_NOT_READY         0x82
#define NVME_SC_RESERVATION_CONFLICT 0x83

/* ---- PRP ---- */
#define NVME_PRP_ENTRIES_PER_PAGE (PAGE_4K_SIZE / 8)

/* ---- Timeouts ---- */
#define NVME_TIMEOUT_MS    5000 /* controller enable timeout (ms) */
#define NVME_TIMEOUT_LOOPS 0x400000

/* ================================================================
 *  Data Structures
 * ================================================================ */

/* 64-byte Submission Queue Entry */
typedef struct {
        uint32_t cdw0; /* OPC[7:0], FUSE[9:8], PSDT[15:14]=00b(PRP), CID[31:16] */
        uint32_t nsid; /* Namespace ID */
        uint32_t rsvd1[2];
        uint64_t prp1;  /* PRP Entry 1 (or data pointer) */
        uint64_t prp2;  /* PRP Entry 2 (or PRP list pointer) */
        uint32_t cdw10; /* Command-specific */
        uint32_t cdw11;
        uint32_t cdw12;
        uint32_t cdw13;
        uint32_t cdw14;
        uint32_t cdw15;
} __attribute__((packed)) nvme_sqe_t;

/* 16-byte Completion Queue Entry */
typedef struct {
        uint32_t dw0;     /* Command-specific */
        uint32_t dw1;     /* Reserved */
        uint16_t sq_head; /* Current SQ head pointer */
        uint16_t sq_id;   /* SQ identifier */
        uint16_t cmd_id;  /* Command ID (CID from submission) */
        uint16_t sfp;     /* Status field: Phase bit[0], SCF[8:1], SFT[11:9] */
} __attribute__((packed)) nvme_cqe_t;

#define NVME_CQE_PHASE(cqe) ((cqe)->sfp & 1)
#define NVME_CQE_SC(cqe)    (((cqe)->sfp >> 1) & 0xFF)
#define NVME_CQE_SCT(cqe)   (((cqe)->sfp >> 9) & 0x7)

/* ---- Identify Controller Data (4096 bytes) ---- */
/* We only define the critical fields; the rest is padding */
typedef struct {
        uint16_t vid;        /* 1:0     PCI Vendor ID        */
        uint16_t ssvid;      /* 3:2     PCI Subsystem Vendor */
        uint8_t  sn[20];     /* 23:4    Serial Number (ASCII) */
        uint8_t  mn[40];     /* 63:24   Model Number (ASCII)  */
        uint8_t  fr[8];      /* 71:64   Firmware Revision     */
        uint8_t  rab;        /* 72      Recommended Arbitration Burst */
        uint8_t  ieee[3];    /* 75:73   IEEE OUI Identifier   */
        uint8_t  cmic;       /* 76      Multi-Path I/O        */
        uint8_t  mdts;       /* 77      Max Data Transfer Size */
        uint16_t cntlid;     /* 79:78   Controller ID         */
        uint32_t ver;        /* 83:80   NVMe Version          */
        uint32_t rtd3r;      /* 87:84   RTD3 Resume Latency   */
        uint32_t rtd3e;      /* 91:88   RTD3 Entry Latency    */
        uint32_t oaes;       /* 95:92   Optional Async Events */
        uint32_t ctratt;     /* 99:96   Controller Attributes */
        uint8_t  rsvd0[12];  /* 111:100                       */
        uint8_t  fguid[16];  /* 127:112 FRU GUID              */
        uint8_t  rsvd1[128]; /* 255:128                       */
        /* Admin Command Set Attributes */
        uint16_t oacs;        /* 257:256 Optional Admin Cmd    */
        uint8_t  acl;         /* 258     Abort Command Limit   */
        uint8_t  aerl;        /* 259     Async Event Req Limit */
        uint8_t  frmw;        /* 260     Firmware Updates      */
        uint8_t  lpa;         /* 261     Log Page Attributes   */
        uint8_t  elpe;        /* 262     Error Log Page Entries */
        uint8_t  npss;        /* 263     Number Power States   */
        uint8_t  avscc;       /* 264     Admin Vendor Specific */
        uint8_t  apsta;       /* 265     Autonomous Power State */
        uint16_t wctemp;      /* 267:266 Warning Composite Temp */
        uint16_t cctemp;      /* 269:268 Critical Composite Temp */
        uint16_t mtfa;        /* 271:270 Max Time Firmware Act  */
        uint32_t hmpre;       /* 275:272 HMB Preferred Size    */
        uint32_t hmmin;       /* 279:276 HMB Minimum Size      */
        uint8_t  tnvmcap[16]; /* 295:280 Total NVM Capacity    */
        uint8_t  unvmcap[16]; /* 311:296 Unallocated NVM Cap   */
        uint32_t rpmbs;       /* 315:312 RPMB Support          */
        uint8_t  rsvd2[196];  /* 511:316                       */
        uint8_t  sqes;        /* 512     SQ Entry Size          */
        uint8_t  cqes;        /* 513     CQ Entry Size          */
        uint16_t maxcmd;      /* 515:514 Max Outstanding Cmds   */
        uint32_t nn;          /* 519:516 Number of Namespaces   */
        uint16_t oncs;        /* 521:520 Optional NVM Cmd Sup   */
        uint16_t fuses;       /* 523:522 Fused Operation Sup    */
        uint8_t  fna;         /* 524     Format NVM Attributes  */
        uint8_t  vwc;         /* 525     Volatile Write Cache   */
        uint16_t awun;        /* 527:526 Atomic Write Unit Norm */
        uint16_t awupf;       /* 529:528 Atomic Write Unit PFail */
        uint8_t  nvscc;       /* 530     NVM Vendor Specific Cmd */
        uint8_t  nwpc;        /* 531     NS Write Protection Cfg */
        uint16_t acwu;        /* 533:532 Atomic Compare & Write */
        uint8_t  rsvd3[2];    /* 535:534                       */
        uint32_t sgls;        /* 539:536 SGL Support           */
        uint32_t mnan;        /* 543:540 Max Number Allowed NS  */
        uint8_t  rsvd4[224];  /* 767:544                       */
        uint8_t  subnqn[256]; /* 1023:768 Subsystem NVMe Qualified Name */
        uint8_t  rsvd5[1024]; /* 2047:1024                     */
        /* Power State Descriptors (32 * 32 bytes) */
        uint8_t psd[1024]; /* 3071:2048                     */
        uint8_t vs[1024];  /* 4095:3072 Vendor Specific     */
} __attribute__((packed)) nvme_identify_ctrl_t;

/* ---- Identify Namespace Data (4096 bytes) ---- */
typedef struct {
        uint64_t nsze;       /* 7:0     Namespace Size (total LBAs) */
        uint64_t ncap;       /* 15:8    Namespace Capacity       */
        uint64_t nuse;       /* 23:16   Namespace Utilization    */
        uint8_t  nsfeat;     /* 24      Namespace Features       */
        uint8_t  nlba;       /* 25      Number of LBA Formats (0-based) */
        uint8_t  flbas;      /* 26      Formatted LBA Size       */
        uint8_t  mc;         /* 27      Metadata Capabilities    */
        uint8_t  dpc;        /* 28      End-to-End Data Protection Cap */
        uint8_t  dps;        /* 29      E2E Data Protection Settings */
        uint8_t  nmic;       /* 30      Multi-path I/O Sharing   */
        uint8_t  rescap;     /* 31      Reservation Capabilities */
        uint8_t  fpi;        /* 32      Format Progress Indicator */
        uint8_t  dlfeat;     /* 33      Deallocate Logical Block  */
        uint16_t nawun;      /* 35:34   NS Atomic Write Unit Norm */
        uint16_t nawupf;     /* 37:36   NS Atomic Write Unit PFail */
        uint16_t nacwu;      /* 39:38   NS Atomic Compare & Write */
        uint16_t nabsn;      /* 41:40   NS Atomic Boundary Norm */
        uint16_t nabo;       /* 43:42   NS Atomic Boundary Offset */
        uint16_t nabspf;     /* 45:44   NS Atomic Boundary PFail */
        uint16_t noiob;      /* 47:46   NS Optimal I/O Boundary */
        uint8_t  nvmcap[16]; /* 63:48   NVM Capacity              */
        uint8_t  rsvd1[40];  /* 103:64                           */
        uint8_t  nguid[16];  /* 119:104 Namespace GUID            */
        uint8_t  eui64[8];   /* 127:120 IEEE Extended Unique Identifier */
        /* LBA Format support: 16 entries × 4 bytes */
        struct {
                uint16_t ms; /* Metadata Size */
                uint8_t  ds; /* LBA Data Size (exponent, 2^ds) */
                uint8_t  rp; /* Relative Performance */
        } lbaf[16];
        uint8_t rsvd2[3904];
} __attribute__((packed)) nvme_identify_ns_t;

/* ---- Namespace descriptor ---- */
typedef struct nvme_namespace {
        uint32_t nsid;
        uint64_t total_sectors;
        uint32_t sector_size;
        uint8_t  ready;
} nvme_namespace_t;

/* ---- Queue Pair descriptor ---- */
typedef struct nvme_queue {
        uint32_t qid;
        uint16_t num_entries;
        uint32_t sq_head; /* host-side head (for bookkeeping) */
        uint32_t sq_tail;
        uint32_t cq_head;
        uint8_t  cq_phase; /* expected phase tag (0 or 1) */

        nvme_sqe_t *sq;      /* HHDM virtual address of SQ */
        nvme_cqe_t *cq;      /* HHDM virtual address of CQ */
        uint64_t    sq_phys; /* physical address of SQ */
        uint64_t    cq_phys; /* physical address of CQ */

        uint32_t sq_pages; /* number of 4K frames for SQ */
        uint32_t cq_pages; /* number of 4K frames for CQ */

        volatile uint32_t *sq_doorbell; /* SQyTDBL MMIO pointer */
        volatile uint32_t *cq_doorbell; /* CQyHDBL MMIO pointer */

        spinlock_t lock;   /* per-queue lock */
        uint32_t   sq_cid; /* monotonically incrementing CID */

        /* Pre-allocated PRP list page (one per queue pair) */
        void    *prp_list_virt;
        uint64_t prp_list_phys;
        uint8_t  prp_list_inuse;
} nvme_queue_t;

/* ---- Controller descriptor ---- */
typedef struct nvme_controller {
        uint8_t             present;
        uint16_t            id;  /* controller index */
        pci_device_cache_t *pci; /* PCI device cache entry */

        volatile void *regs;      /* BAR0 MMIO virtual address */
        uint32_t       stride;    /* doorbell stride in bytes (4 << DSTRD) */
        uint32_t       max_qsize; /* MQES + 1, capped */

        nvme_queue_t admin_q; /* queue pair 0 */

        nvme_namespace_t namespaces[NVME_MAX_NAMESPACES];
        uint32_t         num_namespaces;

        uint32_t irq_vector;

        uint8_t    initialised;
        spinlock_t lock; /* global controller lock */
} nvme_controller_t;

/* ================================================================
 *  Public API
 * ================================================================ */

/* Initialise all NVMe controllers on the PCI bus */
void nvme_init(void);

/* Return the number of discovered controllers */
int nvme_controller_count(void);

/* Return controller at index `i` (or NULL) */
nvme_controller_t *nvme_get_controller(int i);

/* Backend I/O entry points — called through blockdev ops table */
int nvme_read_sectors(const struct blockdev_device *dev, uint32_t lba, uint32_t count, void *buffer);
int nvme_write_sectors(const struct blockdev_device *dev, uint32_t lba, uint32_t count, const void *buffer);

#endif /* INCLUDE_DRIVERS_NVME_H_ */
