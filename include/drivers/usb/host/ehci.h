/*
 *
 *      ehci.h
 *      Enhanced Host Controller Interface register definitions
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_EHCI_H_
#define INCLUDE_EHCI_H_

#include <libs/std/stdint.h>

#define EHCI_PCI_CLASS 0x0c0320

/* EHCI capability register offsets */
#define EHCI_CAP_CAPLENGTH  0x00
#define EHCI_CAP_HCIVERSION 0x02
#define EHCI_CAP_HCSPARAMS  0x04
#define EHCI_CAP_HCCPARAMS  0x08
#define EHCI_CAP_EECP       0x0a

/* EHCI operational register offsets (relative to caplength) */
#define EHCI_OP_USBCMD           0x00
#define EHCI_OP_USBSTS           0x04
#define EHCI_OP_USBINTR          0x08
#define EHCI_OP_FRINDEX          0x0c
#define EHCI_OP_CTRLDSSEGMENT    0x10
#define EHCI_OP_PERIODICLISTBASE 0x14
#define EHCI_OP_ASYNCLISTADDR    0x18
#define EHCI_OP_CONFIGFLAG       0x40
#define EHCI_OP_PORTSC           0x44

#define EHCI_PORT_STRIDE 4

/* USBCMD bits */
#define EHCI_CMD_RUN     (1U << 0)
#define EHCI_CMD_HCRESET (1U << 1)
#define EHCI_CMD_ASENE   (1U << 2)
#define EHCI_CMD_PSEN    (1U << 3)
#define EHCI_CMD_IAAD    (1U << 6)
#define EHCI_CMD_LRESET  (1U << 7)
#define EHCI_CMD_ASYNC   (1U << 8)

/* USBSTS bits */
#define EHCI_STS_INT (1U << 0)
#define EHCI_STS_ERR (1U << 1)
#define EHCI_STS_PCD (1U << 2)
#define EHCI_STS_FLR (1U << 3)
#define EHCI_STS_HSE (1U << 4)
#define EHCI_STS_IAA (1U << 5)
#define EHCI_STS_HCH (1U << 12)
#define EHCI_STS_PSS (1U << 14)
#define EHCI_STS_ASS (1U << 15)

/* USBINTR bits */
#define EHCI_INTR_TX  (1U << 0)
#define EHCI_INTR_ERR (1U << 1)
#define EHCI_INTR_PCD (1U << 2)
#define EHCI_INTR_IAA (1U << 5)

/* PORTSC bits */
#define EHCI_PORT_CCS         (1U << 0)
#define EHCI_PORT_PED         (1U << 1)
#define EHCI_PORT_PR          (1U << 8)
#define EHCI_PORT_PP          (1U << 12)
#define EHCI_PORT_PO          (1U << 13)
#define EHCI_PORT_LS_SHIFT    10
#define EHCI_PORT_LS_MASK     (3U << 10)
#define EHCI_PORT_LS_KSTATE   (1U << 10)
#define EHCI_PORT_PTC_SHIFT   3
#define EHCI_PORT_PTC_MASK    (0x0fU << 3)
/* Bits 26-27 are controller-specific on some implementations; do not use them
 * to infer speed on a standards-compliant EHCI controller. */
#define EHCI_PORT_CSC         (1U << 17)
#define EHCI_PORT_PEC         (1U << 18)
#define EHCI_PORT_CHANGE_BITS (EHCI_PORT_CSC | EHCI_PORT_PEC)

/* HCSPARAMS bits */
#define EHCI_HCS_PORTS_SHIFT 0
#define EHCI_HCS_PORTS_MASK  (0x0fU << 0)
#define EHCI_HCS_PPC         (1U << 4)
#define EHCI_HCS_NCC_SHIFT   8
#define EHCI_HCS_NCC_MASK    (0x0fU << 8)
#define EHCI_HCS_NPCC_SHIFT  12
#define EHCI_HCS_NPCC_MASK   (0x0fU << 12)

/* HCCPARAMS bits */
#define EHCI_HCC_PFL        (1U << 0)
#define EHCI_HCC_AC64       (1U << 1)
#define EHCI_HCC_EECP_SHIFT 8
#define EHCI_HCC_EECP_MASK  (0xffU << 8)

/* Queue Head (for async list and periodic list) */
typedef struct __attribute__((packed, aligned(32))) {
        uint32_t horizontal_link;
        uint32_t endpoint_chars;
        uint32_t endpoint_caps;
        uint32_t current_qtd;
        uint32_t next_qtd;
        uint32_t alt_next_qtd;
        uint32_t token;
        uint32_t buffer[5];
        uint32_t extended[5];
} ehci_qh_t;

/* QH endpoint characteristics bits */
#define EHCI_QH_FA_SHIFT  0
#define EHCI_QH_FA_MASK   (0x7fU << 0)
#define EHCI_QH_EN_SHIFT  8
#define EHCI_QH_EN_MASK   (0x0fU << 8)
#define EHCI_QH_EPS_SHIFT 12
#define EHCI_QH_EPS_MASK  (3U << 12)
#define EHCI_QH_EPS_FULL  (0U << 12)
#define EHCI_QH_EPS_LOW   (1U << 12)
#define EHCI_QH_EPS_HIGH  (2U << 12)
#define EHCI_QH_DTC       (1U << 14)
#define EHCI_QH_H         (1U << 15)
#define EHCI_QH_MPL_SHIFT 16
#define EHCI_QH_MPL_MASK  (0x07ffU << 16)
#define EHCI_QH_CONTROL   (1U << 27)
#define EHCI_QH_NRTL      (1U << 28)

/* QH endpoint capability bits */
#define EHCI_QH_C_SHIFT   0
#define EHCI_QH_C_MASK    (3U << 0)
#define EHCI_QH_RL_SHIFT  4
#define EHCI_QH_RL_MASK   (0x0fU << 4)
#define EHCI_QH_MUL_SHIFT 30
#define EHCI_QH_MUL_MASK  (3U << 30)

/* QH horizontal link pointer bits */
#define EHCI_QHL_TERMINATE (1U << 0)
#define EHCI_QHL_TYPE_QH   (2U << 0)
#define EHCI_QHL_TYPE_ITD  (0U << 0)
#define EHCI_QHL_TYPE_SITD (1U << 0)

/* Queue Element Transfer Descriptor (qTD) */
typedef struct __attribute__((packed, aligned(32))) {
        uint32_t next_qtd;
        uint32_t alt_next_qtd;
        uint32_t token;
        uint32_t buffer[5];
        uint32_t extended[5];
} ehci_qtd_t;

/* qTD token bits */
#define EHCI_QTD_STATUS_MASK  0x000000ffU
#define EHCI_QTD_ACTIVE       (1U << 7)
#define EHCI_QTD_HALTED       (1U << 6)
#define EHCI_QTD_DBE          (1U << 5)
#define EHCI_QTD_BABBLE       (1U << 4)
#define EHCI_QTD_XACTERR      (1U << 3)
#define EHCI_QTD_MISSED       (1U << 2)
#define EHCI_QTD_PID_SHIFT    8
#define EHCI_QTD_PID_MASK     (3U << 8)
#define EHCI_QTD_PID_OUT      (0U << 8)
#define EHCI_QTD_PID_IN       (1U << 8)
#define EHCI_QTD_PID_SETUP    (2U << 8)
#define EHCI_QTD_CERR_SHIFT   10
#define EHCI_QTD_CERR_MASK    (3U << 10)
#define EHCI_QTD_CPAGE_SHIFT  12
#define EHCI_QTD_CPAGE_MASK   (7U << 12)
#define EHCI_QTD_IOC          (1U << 15)
#define EHCI_QTD_TOGGLE       (1U << 31)
#define EHCI_QTD_LENGTH_SHIFT 16
#define EHCI_QTD_LENGTH_MASK  (0x7fffU << 16)

#define EHCI_QTD_NEXT_TERMINATE (1U << 0)

#define EHCI_FRAME_LIST_SIZE 1024

#endif /* INCLUDE_EHCI_H_ */
