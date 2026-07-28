/*
 *
 *      xhci_ring.c
 *      xHCI producer-ring implementation
 *
 *      2026/7/28 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <chipset/common.h>
#include <drivers/xhci.h>
#include <kernel/errno.h>
#include <libs/std/string.h>

int xhci_ring_init(xhci_ring_t *ring, xhci_trb_t *trbs, uint64_t physical, uint16_t count, bool linked)
{
    if (!ring || !trbs || !physical || count < (linked ? 3 : 2) || ((uintptr_t)trbs & 15) || (physical & 15)) return -EINVAL;
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

xhci_trb_t *xhci_ring_enqueue(xhci_ring_t *ring, uint64_t parameter, uint32_t status, uint32_t control, uint64_t *physical)
{
    if (!ring || !ring->trbs || !ring->linked) return NULL;
    spin_lock(&ring->lock);
    if (ring->enqueue == ring->count - 1) {
        xhci_trb_t *link = &ring->trbs[ring->count - 1];
        link->control = XHCI_TRB_TYPE(XHCI_TRB_LINK) | XHCI_TRB_TOGGLE_CYCLE | ring->cycle;
        dma_write_barrier();
        ring->enqueue = 0;
        ring->cycle ^= 1;
    }
    uint16_t index = ring->enqueue++;
    xhci_trb_t *trb = &ring->trbs[index];
    trb->parameter = parameter;
    trb->status    = status;
    trb->control   = (control & ~XHCI_TRB_CYCLE) | ring->cycle;
    dma_write_barrier();
    if (physical) *physical = ring->physical + (uint64_t)index * sizeof(*trb);
    spin_unlock(&ring->lock);
    return trb;
}
