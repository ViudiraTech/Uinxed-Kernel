/*
 *
 *      ahci.h
 *      AHCI SATA/SATAPI common definitions
 *
 *      2026/7/23 By Rainy101112
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_AHCI_AHCI_H_
#define INCLUDE_DRIVERS_AHCI_AHCI_H_

#include <libs/std/stdint.h>

/* SATA signature values */
#define SATA_SIG_ATA   0x00000101
#define SATA_SIG_ATAPI 0xEB140101
#define SATA_SIG_SEMB  0xC33C0101
#define SATA_SIG_PM    0x96690101

/* AHCI device types */
#define AHCI_DEV_NULL   0
#define AHCI_DEV_SATA   1
#define AHCI_DEV_SEMB   2
#define AHCI_DEV_PM     3
#define AHCI_DEV_SATAPI 4

/* Port constants */
#define AHCI_MAX_PORTS   32
#define AHCI_MAX_DEVICES 32
#define AHCI_MAX_CMDS    32
#define AHCI_MAX_SG      8

/* HBA memory register offsets */
#define HOST_CAP        0x00
#define HOST_CTL        0x04
#define HOST_IRQ_STAT   0x08
#define HOST_PORTS_IMPL 0x0c
#define HOST_VERSION    0x10
#define HOST_CAP2       0x24
#define HOST_BOHC       0x28

/* HOST_CTL bits */
#define HOST_RESET   (1u << 0)
#define HOST_IRQ_EN  (1u << 1)
#define HOST_AHCI_EN (1u << 31)

/* HOST_CAP bits */
#define HOST_CAP_64  (1u << 31)
#define HOST_CAP_NCQ (1u << 30)
#define HOST_CAP_SSS (1u << 27)
#define HOST_CAP_PMP (1u << 17)
#define HOST_CAP_FBS (1u << 16)
#define HOST_CAP_CLO (1u << 24)

/* Port register offsets (relative to port base = 0x100 + port_no * 0x80) */
#define PORT_LST_ADDR    0x00
#define PORT_LST_ADDR_HI 0x04
#define PORT_FIS_ADDR    0x08
#define PORT_FIS_ADDR_HI 0x0c
#define PORT_IRQ_STAT    0x10
#define PORT_IRQ_MASK    0x14
#define PORT_CMD         0x18
#define PORT_TFDATA      0x20
#define PORT_SIG         0x24
#define PORT_SSTS        0x28
#define PORT_SCTL        0x2c
#define PORT_SERR        0x30
#define PORT_SACT        0x34
#define PORT_CI          0x38
#define PORT_SNTF        0x3c

/* PORT_CMD bits */
#define PORT_CMD_ST         (1u << 0)
#define PORT_CMD_SPIN_UP    (1u << 1)
#define PORT_CMD_POWER_ON   (1u << 2)
#define PORT_CMD_CLO        (1u << 3)
#define PORT_CMD_FIS_RX     (1u << 4)
#define PORT_CMD_ATAPI      (1u << 24)
#define PORT_CMD_CR         (1u << 15)
#define PORT_CMD_FR         (1u << 14)
#define PORT_CMD_FRE        (1u << 4)
#define PORT_CMD_ICC_ACTIVE (0x1u << 28)

/* PORT_IRQ_STAT bits */
#define PORT_IRQ_TF_ERR      (1u << 30)
#define PORT_IRQ_HBUS_ERR    (1u << 29)
#define PORT_IRQ_IF_ERR      (1u << 27)
#define PORT_IRQ_CONNECT     (1u << 6)
#define PORT_IRQ_PHYRDY      (1u << 22)
#define PORT_IRQ_D2H_REG_FIS (1u << 0)

/* SStatus bits */
#define HBA_PORT_DET_PRESENT 3
#define HBA_PORT_IPM_ACTIVE  1

/* FIS types */
#define FIS_TYPE_REG_H2D   0x27
#define FIS_TYPE_REG_D2H   0x34
#define FIS_TYPE_DMA_ACT   0x39
#define FIS_TYPE_DMA_SETUP 0x41
#define FIS_TYPE_DATA      0x46
#define FIS_TYPE_BIST      0x58
#define FIS_TYPE_PIO_SETUP 0x5F
#define FIS_TYPE_DEV_BITS  0xA1

/* ATA commands */
#define ATA_CMD_READ_DMA_EXT  0x25
#define ATA_CMD_WRITE_DMA_EXT 0x35
#ifndef ATA_CMD_IDENTIFY
#    define ATA_CMD_IDENTIFY 0xEC
#endif
#define ATA_CMD_IDENTIFY_PACKET 0xA1
#define ATA_CMD_PACKET          0xA0
#define ATA_CMD_CACHE_FLUSH     0xE7
#define ATA_CMD_CACHE_FLUSH_EXT 0xEA

/* ATA identify data offsets (words) */
#define ATA_IDENT_DEVICETYPE   0
#define ATA_IDENT_CYLINDERS    2
#define ATA_IDENT_HEADS        6
#define ATA_IDENT_SECTORS      12
#define ATA_IDENT_SERIAL       20
#define ATA_IDENT_MODEL        54
#define ATA_IDENT_CAPABILITIES 98
#define ATA_IDENT_FIELDVALID   106
#define ATA_IDENT_MAX_LBA      120
#define ATA_IDENT_COMMANDSETS  164
#define ATA_IDENT_MAX_LBA_EXT  200

/* FIS Register - Host to Device (20 bytes = 5 DWORDS) */
typedef struct {
        uint8_t fis_type;
        uint8_t pmport : 4;
        uint8_t rsv0   : 3;
        uint8_t c      : 1;
        uint8_t command;
        uint8_t featurel;
        uint8_t lba0;
        uint8_t lba1;
        uint8_t lba2;
        uint8_t device;
        uint8_t lba3;
        uint8_t lba4;
        uint8_t lba5;
        uint8_t featureh;
        uint8_t countl;
        uint8_t counth;
        uint8_t icc;
        uint8_t control;
        uint8_t rsv1[4];
} __attribute__((packed)) fis_reg_h2d_t;

/* FIS Register - Device to Host */
typedef struct {
        uint8_t fis_type;
        uint8_t pmport : 4;
        uint8_t rsv0   : 2;
        uint8_t i      : 1;
        uint8_t rsv1   : 1;
        uint8_t status;
        uint8_t error;
        uint8_t lba0;
        uint8_t lba1;
        uint8_t lba2;
        uint8_t device;
        uint8_t lba3;
        uint8_t lba4;
        uint8_t lba5;
        uint8_t rsv2;
        uint8_t countl;
        uint8_t counth;
        uint8_t rsv3[2];
        uint8_t rsv4[4];
} __attribute__((packed)) fis_reg_d2h_t;

/* FIS DMA Setup */
typedef struct {
        uint8_t  fis_type;
        uint8_t  pmport : 4;
        uint8_t  rsv0   : 1;
        uint8_t  d      : 1;
        uint8_t  i      : 1;
        uint8_t  a      : 1;
        uint8_t  rsved[2];
        uint64_t DMAbufferID;
        uint32_t rsvd;
        uint32_t DMAbufOffset;
        uint32_t TransferCount;
        uint32_t resvd;
} __attribute__((packed)) fis_dma_setup_t;

/* FIS PIO Setup */
typedef struct {
        uint8_t  fis_type;
        uint8_t  pmport : 4;
        uint8_t  rsv0   : 1;
        uint8_t  d      : 1;
        uint8_t  i      : 1;
        uint8_t  rsv1   : 1;
        uint8_t  status;
        uint8_t  error;
        uint8_t  lba0;
        uint8_t  lba1;
        uint8_t  lba2;
        uint8_t  device;
        uint8_t  lba3;
        uint8_t  lba4;
        uint8_t  lba5;
        uint8_t  rsv2;
        uint8_t  countl;
        uint8_t  counth;
        uint8_t  rsv3;
        uint8_t  e_status;
        uint16_t tc;
        uint8_t  rsv4[2];
} __attribute__((packed)) fis_pio_setup_t;

/* FIS Device Bits */
typedef struct {
        uint8_t  fis_type;
        uint8_t  pmport  : 4;
        uint8_t  rsv0    : 2;
        uint8_t  i       : 1;
        uint8_t  n       : 1;
        uint8_t  statusl : 3;
        uint8_t  rsv1    : 1;
        uint8_t  statush : 3;
        uint8_t  rsv2    : 1;
        uint8_t  error;
        uint32_t protocol;
} __attribute__((packed)) fis_dev_bits_t;

/* PRDT entry */
typedef struct {
        uint32_t dba;
        uint32_t dbau;
        uint32_t rsv0;
        uint32_t dbc  : 22;
        uint32_t rsv1 : 9;
        uint32_t i    : 1;
} __attribute__((packed)) hba_prdt_entry_t;

/* Command table (one per command slot) */
typedef struct {
        uint8_t          cfis[64];
        uint8_t          acmd[16];
        uint8_t          rsv[48];
        hba_prdt_entry_t prdt_entry[AHCI_MAX_SG];
} __attribute__((packed)) hba_cmd_tbl_t;

/* Command header */
typedef struct {
        uint8_t           cfl  : 5;
        uint8_t           a    : 1;
        uint8_t           w    : 1;
        uint8_t           p    : 1;
        uint8_t           r    : 1;
        uint8_t           b    : 1;
        uint8_t           c    : 1;
        uint8_t           rsv0 : 1;
        uint8_t           pmp  : 4;
        uint16_t          prdtl;
        volatile uint32_t prdbc;
        uint32_t          ctba;
        uint32_t          ctbau;
        uint32_t          rsv1[4];
} __attribute__((packed)) hba_cmd_header_t;

/* Received FIS structure */
typedef struct {
        fis_dma_setup_t dsfis;
        uint8_t         pad0[4];
        fis_pio_setup_t psfis;
        uint8_t         pad1[12];
        fis_reg_d2h_t   rfis;
        uint8_t         pad2[4];
        fis_dev_bits_t  sdbfis;
        uint8_t         ufis[64];
        uint8_t         rsv[0x100 - 0xA0];
} __attribute__((packed)) hba_fis_t;

/* HBA memory mapped registers (first part) */
typedef volatile struct {
        uint32_t cap;
        uint32_t ghc;
        uint32_t is;
        uint32_t pi;
        uint32_t vs;
        uint32_t ccc_ctl;
        uint32_t ccc_pts;
        uint32_t em_loc;
        uint32_t em_ctl;
        uint32_t cap2;
        uint32_t bohc;
        uint8_t  rsv[0xA0 - 0x2C];
        uint8_t  vendor[0x100 - 0xA0];
} hba_mem_t;

/* Port memory registers */
typedef volatile struct {
        uint32_t clb;
        uint32_t clbu;
        uint32_t fb;
        uint32_t fbu;
        uint32_t is;
        uint32_t ie;
        uint32_t cmd;
        uint32_t rsv0;
        uint32_t tfd;
        uint32_t sig;
        uint32_t ssts;
        uint32_t sctl;
        uint32_t serr;
        uint32_t sact;
        uint32_t ci;
        uint32_t sntf;
        uint32_t fbs;
        uint32_t rsv1[11];
        uint32_t vendor[4];
} hba_port_t;

/* AHCI device structure (shared between SATA and SATAPI) */
typedef struct {
        uint8_t  reserved;
        uint8_t  type;
        uint8_t  port;
        uint32_t size;
        uint32_t sector_size;
        char     model[41];
} ahci_device_t;

/* External declarations */
extern ahci_device_t ahci_devices[AHCI_MAX_DEVICES];
extern int           ahci_device_count;

/* AHCI MMIO helpers (shared with satapi) */
uint32_t ahci_read32(volatile uint8_t *base, uint32_t reg);
void     ahci_write32(volatile uint8_t *base, uint32_t reg, uint32_t val);

/* HBA MMIO base (shared with satapi) */
extern volatile uint8_t *hba_mmio;

/* Per-port state (shared between ahci.c and satapi.c) */
typedef struct {
        volatile uint8_t *port_mmio;
        uint8_t           port_no;
        int               present;
        hba_cmd_header_t *cmd_list;
        hba_fis_t        *fis;
        hba_cmd_tbl_t    *cmd_tbl;
        uint64_t          clb_phys;
        uint64_t          fb_phys;
        uint64_t          ct_phys;
        uint8_t          *dma_buf;
        uint64_t          dma_buf_phys;
} ahci_port_state_t;

extern ahci_port_state_t ahci_ports[AHCI_MAX_PORTS];

/* AHCI SATA API */
void init_ahci(void);
int  ahci_read_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, void *buffer);
int  ahci_write_sectors(uint8_t drive, uint8_t numsects, uint32_t lba, const void *buffer);

#endif
