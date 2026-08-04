/*
 *
 *      uhci.h
 *      Universal Host Controller Interface register definitions
 *
 *      2026/7/29 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_UHCI_H_
#define INCLUDE_UHCI_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

#define UHCI_PCI_CLASS 0x0c0300

/* UHCI I/O register offsets */
#define UHCI_USBCMD    0x00
#define UHCI_USBSTS    0x02
#define UHCI_USBINTR   0x04
#define UHCI_FRNUM     0x06
#define UHCI_FLBASEADD 0x08
#define UHCI_SOFMOD    0x0c
#define UHCI_PORTSC1   0x10
#define UHCI_PORTSC2   0x12

/* USBCMD bits */
#define UHCI_CMD_RS      (1U << 0)
#define UHCI_CMD_HCRESET (1U << 1)
#define UHCI_CMD_GRESET  (1U << 2)
#define UHCI_CMD_EGSM    (1U << 3)
#define UHCI_CMD_FGR     (1U << 4)
#define UHCI_CMD_SWDBG   (1U << 5)
#define UHCI_CMD_CF      (1U << 6)
#define UHCI_CMD_MAXP    (1U << 7)

/* USBSTS bits */
#define UHCI_STS_USBINT (1U << 0)
#define UHCI_STS_ERROR  (1U << 1)
#define UHCI_STS_RD     (1U << 2)
#define UHCI_STS_HSE    (1U << 3)
#define UHCI_STS_HCPE   (1U << 4)
#define UHCI_STS_HCH    (1U << 5)

/* USBINTR bits */
#define UHCI_INTR_TIMEOUT (1U << 0)
#define UHCI_INTR_RESUME  (1U << 1)
#define UHCI_INTR_IOC     (1U << 2)
#define UHCI_INTR_SP      (1U << 3)

/* PORTSC bits */
#define UHCI_PORTSC_CCS         (1U << 0)
#define UHCI_PORTSC_CSC         (1U << 1)
#define UHCI_PORTSC_PED         (1U << 2)
#define UHCI_PORTSC_PEC         (1U << 3)
#define UHCI_PORTSC_LSDA        (1U << 8)
#define UHCI_PORTSC_PR          (1U << 9)
#define UHCI_PORTSC_SUSP        (1U << 12)
#define UHCI_PORTSC_RD          (1U << 13)
#define UHCI_PORTSC_CHANGE_BITS (UHCI_PORTSC_CSC | UHCI_PORTSC_PEC)

#define UHCI_FRAME_LIST_SIZE 1024
#define UHCI_MAX_PORTS       2
#define UHCI_NUM_TD          64
#define UHCI_NUM_QH          32

/* Transfer Descriptor */
typedef struct __attribute__((packed, aligned(16))) {
        uint32_t link;
        uint32_t control_status;
        uint32_t token;
        uint32_t buffer;
} uhci_td_t;

/* Queue Head */
typedef struct __attribute__((packed, aligned(16))) {
        uint32_t horizontal_link;
        uint32_t element_link;
        uint32_t reserved[2];
} uhci_qh_t;

/* TD control/status bits */
#define UHCI_TD_ACTIVE       (1U << 23)
#define UHCI_TD_IOC          (1U << 24)
#define UHCI_TD_LOW_SPEED    (1U << 26)
#define UHCI_TD_ERROR_COUNT  (3U << 27)
#define UHCI_TD_SHORT_PACKET (1U << 29)
#define UHCI_TD_STALLED      (1U << 22)
#define UHCI_TD_DBUFERR      (1U << 21)
#define UHCI_TD_BABBLE       (1U << 20)
#define UHCI_TD_NAK          (1U << 19)
#define UHCI_TD_CRCTIMEO     (1U << 18)
#define UHCI_TD_BITSTUFF     (1U << 17)
#define UHCI_TD_ACTLEN_MASK  0x000007ff

/* TD token bits */
#define UHCI_TOKEN_PID_SHIFT     0
#define UHCI_TOKEN_PID_MASK      (0xffU << 0)
#define UHCI_TOKEN_DEVADDR_SHIFT 8
#define UHCI_TOKEN_DEVADDR_MASK  (0x7fU << 8)
#define UHCI_TOKEN_ENDP_SHIFT    15
#define UHCI_TOKEN_ENDP_MASK     (0x0fU << 15)
#define UHCI_TOKEN_TOGGLE        (1U << 19)
#define UHCI_TOKEN_TOGGLE_SHIFT  19
#define UHCI_TOKEN_MAXLEN_SHIFT  21
#define UHCI_TOKEN_MAXLEN_MASK   (0x7ffU << 21)

#define UHCI_PID_SETUP 0x2d
#define UHCI_PID_IN    0x69
#define UHCI_PID_OUT   0xe1

/* Link pointer bits */
#define UHCI_LINK_TERMINATE (1U << 0)
#define UHCI_LINK_QH        (1U << 1)
#define UHCI_LINK_DEPTH     (1U << 2)
#define UHCI_LINK_MASK      0xfffffff0U

static inline uint32_t uhci_td_encode_length(size_t length)
{
    return length ? (uint32_t)(length - 1) & 0x7ffU : 0x7ffU;
}

static inline size_t uhci_td_decode_length(uint32_t control_status)
{
    uint32_t encoded = control_status & UHCI_TD_ACTLEN_MASK;
    return encoded == 0x7ffU ? 0 : (size_t)encoded + 1;
}

#endif /* INCLUDE_UHCI_H_ */
