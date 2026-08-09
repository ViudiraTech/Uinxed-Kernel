/*
 *
 *      ohci.h
 *      Open Host Controller Interface register definitions
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_OHCI_H_
#define INCLUDE_OHCI_H_

#include <libs/std/stdint.h>

#define OHCI_PCI_CLASS 0x0c0310

/* OHCI MMIO register offsets */
#define OHCI_HcRevision         0x00
#define OHCI_HcControl          0x04
#define OHCI_HcCommandStatus    0x08
#define OHCI_HcInterruptStatus  0x0c
#define OHCI_HcInterruptEnable  0x10
#define OHCI_HcInterruptDisable 0x14
#define OHCI_HcHCCA             0x18
#define OHCI_HcPeriodCurrentED  0x20
#define OHCI_HcControlHeadED    0x24
#define OHCI_HcControlCurrentED 0x28
#define OHCI_HcBulkHeadED       0x2c
#define OHCI_HcBulkCurrentED    0x30
#define OHCI_HcDoneHead         0x34
#define OHCI_HcFmInterval       0x38
#define OHCI_HcFmRemaining      0x3c
#define OHCI_HcFmNumber         0x40
#define OHCI_HcPeriodicStart    0x44
#define OHCI_HcLSThreshold      0x48
#define OHCI_HcRhDescriptorA    0x4c
#define OHCI_HcRhDescriptorB    0x50
#define OHCI_HcRhStatus         0x54
#define OHCI_HcRhPortStatus     0x58

/* HcControl bits */
#define OHCI_CTRL_CBSR_SHIFT 0
#define OHCI_CTRL_CBSR_MASK  (3U << 0)
#define OHCI_CTRL_PLE        (1U << 2)
#define OHCI_CTRL_IE         (1U << 3)
#define OHCI_CTRL_CLE        (1U << 4)
#define OHCI_CTRL_BLE        (1U << 5)
#define OHCI_CTRL_HCFS_SHIFT 6
#define OHCI_CTRL_HCFS_MASK  (3U << 6)
#define OHCI_CTRL_HCFS_RESET 0
#define OHCI_CTRL_HCFS_OPER  (2U << 6)
#define OHCI_CTRL_HCFS_SUSP  (3U << 6)
#define OHCI_CTRL_IR         (1U << 8)
#define OHCI_CTRL_RWC        (1U << 9)
#define OHCI_CTRL_RWE        (1U << 10)

/* HcCommandStatus bits */
#define OHCI_CMD_HCR       (1U << 0)
#define OHCI_CMD_CLF       (1U << 1)
#define OHCI_CMD_BLF       (1U << 2)
#define OHCI_CMD_OCR       (1U << 3)
#define OHCI_CMD_SOC_SHIFT 16
#define OHCI_CMD_SOC_MASK  (3U << 16)

/* HcInterruptStatus / Enable bits */
#define OHCI_INTR_SO   (1U << 0)
#define OHCI_INTR_WDH  (1U << 1)
#define OHCI_INTR_SF   (1U << 2)
#define OHCI_INTR_RD   (1U << 3)
#define OHCI_INTR_UE   (1U << 4)
#define OHCI_INTR_FNO  (1U << 5)
#define OHCI_INTR_RHSC (1U << 6)
#define OHCI_INTR_OC   (1U << 7)
#define OHCI_INTR_MIE  (1U << 31)

/* HcRhStatus bits */
#define OHCI_RHS_LPSC (1U << 0)
#define OHCI_RHS_POCI (1U << 1)
#define OHCI_RHS_OCI  (1U << 17)
#define OHCI_RHS_LPS  (1U << 16)
#define OHCI_RHS_DRWA (1U << 17)

/* HcRhPortStatus bits */
#define OHCI_PORT_CCS         (1U << 0)
#define OHCI_PORT_PES         (1U << 1)
#define OHCI_PORT_PSS         (1U << 2)
#define OHCI_PORT_POCI        (1U << 3)
#define OHCI_PORT_PRS         (1U << 4)
#define OHCI_PORT_PPS         (1U << 8)
#define OHCI_PORT_LSDA        (1U << 9)
#define OHCI_PORT_CSC         (1U << 16)
#define OHCI_PORT_PESC        (1U << 17)
#define OHCI_PORT_PSSC        (1U << 18)
#define OHCI_PORT_OCIC        (1U << 19)
#define OHCI_PORT_PRSC        (1U << 20)
#define OHCI_PORT_CHANGE_BITS 0x001f0000U

/* Endpoint Descriptor (ED) */
typedef struct __attribute__((packed, aligned(16))) {
        uint32_t control;
        uint32_t tail_pointer;
        uint32_t head_pointer;
        uint32_t next_ed;
} ohci_ed_t;

/* ED control bits */
#define OHCI_ED_FA_SHIFT  0
#define OHCI_ED_FA_MASK   (0x7fU << 0)
#define OHCI_ED_EN_SHIFT  7
#define OHCI_ED_EN_MASK   (0x0fU << 7)
#define OHCI_ED_D_SHIFT   11
#define OHCI_ED_D_MASK    (3U << 11)
#define OHCI_ED_D_OUT     (1U << 11)
#define OHCI_ED_D_IN      (2U << 11)
#define OHCI_ED_S         (1U << 13)
#define OHCI_ED_K         (1U << 14)
#define OHCI_ED_F         (1U << 15)
#define OHCI_ED_MPS_SHIFT 16
#define OHCI_ED_MPS_MASK  (0x7ffU << 16)

/* General Transfer Descriptor (TD) */
typedef struct __attribute__((packed, aligned(16))) {
        uint32_t control;
        uint32_t current_buffer_pointer;
        uint32_t next_td;
        uint32_t buffer_end;
} ohci_gtd_t;

/* TD control bits */
#define OHCI_TD_R                  (1U << 18)
#define OHCI_TD_DP_SHIFT           19
#define OHCI_TD_DP_MASK            (3U << 19)
#define OHCI_TD_DP_SETUP           (0U << 19)
#define OHCI_TD_DP_OUT             (1U << 19)
#define OHCI_TD_DP_IN              (2U << 19)
#define OHCI_TD_DI_SHIFT           21
#define OHCI_TD_DI_MASK            (7U << 21)
#define OHCI_TD_DI_NONE            (7U << 21)
#define OHCI_TD_TOGGLE_CARRY       (0U << 24)
#define OHCI_TD_TOGGLE_DATA0       (2U << 24)
#define OHCI_TD_TOGGLE_DATA1       (3U << 24)
#define OHCI_TD_EC_SHIFT           26
#define OHCI_TD_EC_MASK            (3U << 26)
#define OHCI_TD_CC_SHIFT           28
#define OHCI_TD_CC_MASK            (0x0fU << 28)
#define OHCI_TD_CC_NOERROR         0
#define OHCI_TD_CC_CRC             1
#define OHCI_TD_CC_BITSTUFF        2
#define OHCI_TD_CC_TOGGLE          3
#define OHCI_TD_CC_STALL           4
#define OHCI_TD_CC_NORESPONSE      5
#define OHCI_TD_CC_PID             6
#define OHCI_TD_CC_UNEXPECTED_PID  7
#define OHCI_TD_CC_DATA_OVERRUN    8
#define OHCI_TD_CC_DATA_UNDERRUN   9
#define OHCI_TD_CC_BUFFER_OVERRUN  12
#define OHCI_TD_CC_BUFFER_UNDERRUN 13
#define OHCI_TD_CC_NOT_ACCESSED    15

/* HCCA (Host Controller Communication Area) */
typedef struct __attribute__((packed, aligned(256))) {
        uint32_t interrupt_table[32];
        uint16_t frame_number;
        uint16_t pad;
        uint32_t done_head;
        uint8_t  reserved[116];
} ohci_hcca_t;

#endif /* INCLUDE_OHCI_H_ */
