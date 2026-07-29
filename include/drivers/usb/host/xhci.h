/*
 *
 *      xhci.h
 *      eXtensible Host Controller Interface definitions
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_XHCI_H_
#define INCLUDE_XHCI_H_

#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

#define XHCI_TRB_CYCLE        (1U << 0)
#define XHCI_TRB_TOGGLE_CYCLE (1U << 1)
#define XHCI_TRB_CHAIN        (1U << 4)
#define XHCI_TRB_IOC          (1U << 5)
#define XHCI_TRB_IDT          (1U << 6)
#define XHCI_TRB_DIR_IN       (1U << 16)
#define XHCI_TRB_TYPE_SHIFT   10
#define XHCI_TRB_TYPE_MASK    (0x3fU << XHCI_TRB_TYPE_SHIFT)
#define XHCI_TRB_TYPE(value)  ((uint32_t)(value) << XHCI_TRB_TYPE_SHIFT)

enum xhci_trb_type {
    XHCI_TRB_NORMAL             = 1,
    XHCI_TRB_SETUP_STAGE        = 2,
    XHCI_TRB_DATA_STAGE         = 3,
    XHCI_TRB_STATUS_STAGE       = 4,
    XHCI_TRB_LINK               = 6,
    XHCI_TRB_ENABLE_SLOT        = 9,
    XHCI_TRB_DISABLE_SLOT       = 10,
    XHCI_TRB_ADDRESS_DEVICE     = 11,
    XHCI_TRB_CONFIGURE_ENDPOINT = 12,
    XHCI_TRB_EVALUATE_CONTEXT   = 13,
    XHCI_TRB_RESET_ENDPOINT     = 14,
    XHCI_TRB_STOP_ENDPOINT      = 15,
    XHCI_TRB_SET_TR_DEQUEUE     = 16,
    XHCI_TRB_COMMAND_COMPLETION = 33,
    XHCI_TRB_PORT_STATUS_CHANGE = 34,
    XHCI_TRB_TRANSFER_EVENT     = 32,
};

typedef struct __attribute__((packed, aligned(16))) {
        uint64_t parameter;
        uint32_t status;
        uint32_t control;
} xhci_trb_t;

typedef struct {
        xhci_trb_t *trbs;
        uint64_t    physical;
        uint16_t    count;
        uint16_t    enqueue;
        uint8_t     cycle;
        bool        linked;
        spinlock_t  lock;
} xhci_ring_t;

int         xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *trbs, uint64_t physical, uint16_t count, bool linked);
xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter, uint32_t status, uint32_t control, uint64_t *physical);

#endif /* INCLUDE_XHCI_H_ */
