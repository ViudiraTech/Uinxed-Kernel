/*
 *
 *      xhci_ring.c
 *      xHCI producer-ring implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/usb/host/xhci/xhci.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/string.h>

/* Initialize a TRB ring, installing a LINK TRB when the ring is linked. */
int xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *trbs, uint64_t physical, uint16_t count, bool linked)
{
    if (!ring || !trbs || !physical || count < (linked ? 3 : 2) || ((uintptr_t)trbs & 15) || (physical & 15)) {
        plogk("usb-xhci: ring_init: invalid argument (count=%u, linked=%u)\n", (unsigned)count, (unsigned)linked);
        return -EINVAL;
    }
    memset(ring, 0, sizeof(*ring));
    memset(trbs, 0, (size_t)count * sizeof(*trbs));
    ring->trbs     = trbs;
    ring->physical = physical;
    ring->count    = count;
    ring->cycle    = 1;
    ring->linked   = linked;
    if (linked) {
        trbs[count - 1].parameter = physical;
        trbs[count - 1].control   = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | XHCI_TRB_CYCLE;
    }
    return EOK;
}

/* Append a TRB, wrapping through the LINK entry when the ring is full. */
xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter, uint32_t status, uint32_t control, uint64_t *physical)
{
    if (!ring || !ring->trbs || !ring->linked) {
        plogk("usb-xhci: ring_enqueue: enqueue on invalid ring.\n");
        return NULL;
    }
    spin_lock(&ring->lock);
    if (ring->enqueue == ring->count - 1) {
        xhci_trb_t *link = &ring->trbs[ring->count - 1];
        link->control    = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | ring->cycle;
        dma_write_barrier();
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    uint16_t    index = ring->enqueue++;
    xhci_trb_t *trb   = &ring->trbs[index];
    trb->parameter    = parameter;
    trb->status       = status;
    trb->control      = (control & ~XHCI_TRB_CYCLE) | ring->cycle;
    dma_write_barrier();
    if (physical) *physical = ring->physical + (uint64_t)index * sizeof(*trb);
    spin_unlock(&ring->lock);
    return trb;
}

/* Clear a completed TRB slot so the controller may reuse it. */
static void xhci_ring_dequeue(xhci_ring_t *ring, uint16_t index)
{
    if (!ring || !ring->trbs || index >= ring->count) {
        plogk("usb-xhci: ring_dequeue: dequeue out of range (index=%u, count=%u)\n", (unsigned)index, ring ? (unsigned)ring->count : 0);
        return;
    }
    spin_lock(&ring->lock);
    if (index < ring->enqueue) {
        xhci_trb_t *trb = &ring->trbs[index];
        trb->parameter  = 0;
        trb->status     = 0;
        trb->control    = 0;
    }
    spin_unlock(&ring->lock);
}
