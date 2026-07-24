/*
 *
 *      pci.c
 *      VirtIO PCI transport layer
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Implements the virtio-over-PCI transport for all virtio devices in the
 *  system.  Only modern (virtio 1.0+) with PCI capabilities is supported;
 *  legacy-mode devices are not probed.  This file provides MMIO-based
 *  capability discovery, common-config / notify / ISR / device-config
 *  accessors, virtqueue ring setup, feature negotiation, and device
 *  reset/status management.
 *
 */

#include <chipset/common.h>
#include <drivers/pci.h>
#include <drivers/virt/pci.h>
#include <kernel/debug.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/stdlib.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/hhdm.h>
#include <mem/page.h>

/* ------------------------------------------------------------------ */
/* Logging prefix                                                      */
/* ------------------------------------------------------------------ */

#define VP_LOG(fmt, ...) plogk("vp: " fmt, ##__VA_ARGS__)
#define VP_ERR(fmt, ...) plogk("vp: [error] " fmt, ##__VA_ARGS__)
#define VP_DBG(fmt, ...) plogk("vp: [debug] " fmt, ##__VA_ARGS__)

/* ------------------------------------------------------------------ */
/* Capability parsing — walk the PCI vendor-defined capability list    */
/* ------------------------------------------------------------------ */

/*
 * Return the MMIO virtual address for a given PCI capability.
 * Maps the BAR fully (if not yet mapped) and returns bar_base + cap->offset.
 */
static volatile void *vp_map_cap_bar(struct vp_device *dev, struct vp_cap *cap)
{
    pci_device_reg_t reg = {dev->pci_dev, 0};
    uint32_t         bar_raw;
    uint64_t         bar_phys;
    uint64_t         map_start, map_len;

    /* Read BAR physical address directly from PCI config space */
    reg.offset = 0x10 + 4 * cap->bar;
    bar_raw    = read_pci(reg);
    bar_phys   = bar_raw & ~0xfULL;

    /* Handle 64-bit BAR: lower 32 bits may be 0 if mapped above 4GB */
    if ((bar_raw & 0x6) == 0x4) {
        reg.offset = 0x10 + 4 * (cap->bar + 1);
        bar_phys |= (uint64_t)read_pci(reg) << 32;
    }

    if (!bar_phys) {
        VP_ERR("BAR %u for device %04x:%04x has null address\n", cap->bar, dev->vendor_id, dev->device_id);
        return NULL;
    }

    /* Map the needed pages into the HHDM window.
     * phys_to_virt provides the virtual address (HHDM offset),
     * but the page-table entries may be absent for MMIO regions.
     * page_map_range_to maps phys→virt for contiguously addressed pages.
     *
     * CRITICAL: MMIO BARs must be mapped uncacheable (PTE_PCD).
     * Without PCD, the CPU caches all MMIO accesses (Write-Back),
     * so writes to device registers never reach the PCI bus and
     * the device never sees status updates, queue configs, or
     * notify doorbell kicks. */
    map_start = bar_phys & ~(uint64_t)0xfff;
    map_len   = ((bar_phys + cap->offset + cap->length + 0xfff) & ~(uint64_t)0xfff) - map_start;

    page_map_range_to(get_kernel_pagedir(), map_start, map_len, PTE_MMIO_FLAGS);

    return (volatile void *)((uintptr_t)phys_to_virt(bar_phys) + cap->offset);
}

static int vp_scan_caps(struct vp_device *dev)
{
    pci_device_cache_t *pci = dev->pci_dev;
    pci_device_reg_t    reg = {pci, 0x34};
    uint32_t            cap_off;
    int                 found_common = 0, found_notify = 0, found_isr = 0;

    reg.offset = 0x34;
    cap_off    = read_pci(reg) & 0xfc;

    if (!cap_off) {
        VP_ERR("No PCI capabilities found (not a modern virtio device)\n");
        return -ENODEV;
    }

    while (cap_off) {
        struct vp_cap cap;
        uint32_t      cap_data[4];
        int           i;

        /*
         * Read the full virtio_pci_cap (16 bytes):
         *   dword 0: cap_vndr(1) + cap_next(1) + cap_len(1) + cfg_type(1)
         *   dword 1: bar(1) + padding(3)
         *   dword 2: offset (le32)
         *   dword 3: length (le32)
         */
        for (i = 0; i < 4; i++) {
            reg.offset  = cap_off + i * 4;
            cap_data[i] = read_pci(reg);
        }

        cap.cap_vndr = cap_data[0] & 0xff;
        cap.cap_next = (cap_data[0] >> 8) & 0xff;
        cap.cfg_type = (cap_data[0] >> 24) & 0xff;
        cap.bar      = cap_data[1] & 0xff;
        cap.offset   = cap_data[2];
        cap.length   = cap_data[3];

        if (cap.cap_vndr != 0x09) {
            cap_off = cap.cap_next;
            continue;
        }

        switch (cap.cfg_type) {
            case VIRTIO_PCI_CAP_COMMON_CFG :
                dev->common_cap = cap;
                dev->common     = vp_map_cap_bar(dev, &cap);
                if (dev->common) {
                    found_common = 1;
                    VP_DBG("common cfg at BAR%u+0x%x (len %u)\n", cap.bar, cap.offset, cap.length);
                }
                break;

            case VIRTIO_PCI_CAP_NOTIFY_CFG : {
                struct vp_notify_cap ncap;
                uint32_t             ndata;
                /* notify_off_multiplier is at cap_off + 16 */
                reg.offset = cap_off + 16;
                ndata      = read_pci(reg);

                ncap.cap                   = cap;
                ncap.notify_off_multiplier = ndata;
                dev->notify_cap            = ncap;
                dev->notify_base           = vp_map_cap_bar(dev, &cap);
                dev->notify_off_multiplier = ndata;
                found_notify               = 1;
                VP_DBG("notify cfg at BAR%u+0x%x (mult %u)\n", cap.bar, cap.offset, ndata);
                break;
            }

            case VIRTIO_PCI_CAP_ISR_CFG :
                dev->isr_cap = cap;
                dev->isr     = vp_map_cap_bar(dev, &cap);
                if (dev->isr) {
                    found_isr = 1;
                    VP_DBG("isr cfg at BAR%u+0x%x\n", cap.bar, cap.offset);
                }
                break;

            case VIRTIO_PCI_CAP_DEVICE_CFG :
                dev->device_cap = cap;
                dev->device_cfg = vp_map_cap_bar(dev, &cap);
                if (dev->device_cfg) { VP_DBG("device cfg at BAR%u+0x%x (len %u)\n", cap.bar, cap.offset, cap.length); }
                break;
        }

        cap_off = cap.cap_next;
    }

    if (!found_common || !found_notify || !found_isr) {
        VP_ERR("Missing required capabilities (common=%d notify=%d isr=%d)\n", found_common, found_notify, found_isr);
        return -ENODEV;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Device status management                                            */
/* ------------------------------------------------------------------ */

void vp_set_status(struct vp_device *dev, uint8_t status)
{
    if (dev->common) { dev->common->device_status = status; }
}

uint8_t vp_get_status(struct vp_device *dev)
{
    if (!dev->common) { return 0xff; }
    return dev->common->device_status;
}

void vp_reset_device(struct vp_device *dev)
{
    vp_set_status(dev, VIRTIO_STATUS_RESET);
    /* Read back to flush the write */
    vp_get_status(dev);
}

/* ------------------------------------------------------------------ */
/* Feature negotiation                                                 */
/* ------------------------------------------------------------------ */

int vp_negotiate_features(struct vp_device *dev, uint64_t guest_features, uint64_t *negotiated)
{
    uint32_t lo, hi;

    if (!dev->common) { return -ENODEV; }

    /* Device features (must be read in two phases) */
    dev->common->device_feature_select = 0;
    lo                                 = dev->common->device_feature;
    dev->common->device_feature_select = 1;
    hi                                 = dev->common->device_feature;

    VP_DBG("Device features: 0x%016llx\n", ((uint64_t)hi << 32) | lo);

    /* Mask with guest-requested features */
    lo &= (uint32_t)(guest_features);
    hi &= (uint32_t)(guest_features >> 32);

    /* Write back driver features */
    dev->common->driver_feature_select = 0;
    dev->common->driver_feature        = lo;
    dev->common->driver_feature_select = 1;
    dev->common->driver_feature        = hi;

    dev->features = ((uint64_t)hi << 32) | lo;
    if (negotiated) { *negotiated = dev->features; }

    VP_DBG("Negotiated features: 0x%016llx\n", dev->features);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Device configuration space accessors                                */
/* ------------------------------------------------------------------ */

void vp_read_device_config(struct vp_device *dev, void *buf, int offset, int len)
{
    volatile uint8_t *cfg = dev->device_cfg;
    uint8_t          *dst = (uint8_t *)buf;
    int               i;

    if (!cfg) { return; }
    for (i = 0; i < len; i++) { dst[i] = cfg[offset + i]; }
}

void vp_write_device_config(struct vp_device *dev, const void *buf, int offset, int len)
{
    volatile uint8_t *cfg = dev->device_cfg;
    const uint8_t    *src = (const uint8_t *)buf;
    int               i;

    if (!cfg) { return; }
    for (i = 0; i < len; i++) { cfg[offset + i] = src[i]; }
}

/* ------------------------------------------------------------------ */
/* Virtqueue ring setup                                                */
/* ------------------------------------------------------------------ */

/*
 * Allocate and initialise a single virtqueue.
 * num must be a power of two.
 */
int vp_setup_vq(struct vp_device *dev, int index, int num, struct vp_virtqueue *vq)
{
    int                            alloc_size;
    int                            i;
    struct vp_common_cfg volatile *common = dev->common;

    if (!common || index >= common->num_queues) { return -EINVAL; }

    /* Select the queue */
    common->queue_select = index;
    if (!common->queue_size) {
        VP_ERR("Queue %d size is 0\n", index);
        return -ENODEV;
    }

    if (num > common->queue_size) { num = common->queue_size; }
    if (num & (num - 1)) {
        /* round down to power of two */
        while (num & (num - 1)) { num &= num - 1; }
    }
    if (num < 2) { num = 2; }

    /*
     * Write the actual queue size back to the device before configuring
     * the rest of the queue.  VirtIO spec §4.1.4.3.1: "The driver MAY
     * write a smaller value to inform the device that it will use fewer
     * descriptors."  QEMU requires this for correct operation.
     */
    common->queue_size = num;

    memset(vq, 0, sizeof(*vq));
    vq->index            = index;
    vq->num_max          = num;
    vq->num_free         = num;
    vq->free_head        = 0;
    vq->avail_idx_shadow = 0;
    vq->used_idx         = 0;
    vq->lock.lock        = 0;
    vq->vp               = dev;
    vq->notify_off       = common->queue_notify_off;

    /* Allocate descriptor table, available ring, used ring as one block */
    alloc_size = num * sizeof(struct vring_desc) + sizeof(struct vring_avail) + num * sizeof(uint16_t) + sizeof(struct vring_used)
                 + num * sizeof(struct vring_used_elem);
    /* Align to page */
    alloc_size = (alloc_size + 4095) & ~4095;

    vq->queue_mem = malloc(alloc_size);
    if (!vq->queue_mem) { return -ENOMEM; }
    memset(vq->queue_mem, 0, alloc_size);

    vq->desc  = (struct vring_desc *)vq->queue_mem;
    vq->avail = (struct vring_avail *)((uint8_t *)vq->desc + num * sizeof(struct vring_desc));
    vq->used  = (struct vring_used *)((uint8_t *)vq->avail + sizeof(struct vring_avail) + num * sizeof(uint16_t));

    /* Initialize free descriptor list */
    vq->free_descs = malloc(num * sizeof(uint16_t));
    vq->desc_data  = malloc(num * sizeof(void *));
    if (!vq->free_descs || !vq->desc_data) {
        free(vq->free_descs);
        free(vq->desc_data);
        free(vq->queue_mem);
        return -ENOMEM;
    }

    /*
     * Build the initial LIFO free-descriptor chain.
     * free_descs[i] stores the NEXT free descriptor index after i.
     * Allocation pops from free_head, deallocation pushes back:
     *   alloc:  head = free_head; free_head = free_descs[head]; num_free--;
     *   free:   free_descs[head] = free_head; free_head = head;  num_free++;
     *
     * Initial chain: free_head → 0 → 1 → 2 → ... → num-1
     */
    for (i = 0; i < num; i++) { vq->free_descs[i] = (uint16_t)(i + 1); }
    /* The last descriptor has no next — its free_descs entry is never
     * read until it has first been pushed back (which overwrites it). */

    /*
     * Program the queue into common config (physical addresses).
     * Write barrier before queue_enable: the device must see the
     * descriptor/avail/used addresses before the enable flag.
     * VirtIO spec §4.1.4.3.1: "The driver MUST write the queue
     * address registers before setting the queue_enable bit."
     */
    common->queue_desc  = (uintptr_t)virt_any_to_phys((uintptr_t)vq->desc);
    common->queue_avail = (uintptr_t)virt_any_to_phys((uintptr_t)vq->avail);
    common->queue_used  = (uintptr_t)virt_any_to_phys((uintptr_t)vq->used);
    compiler_barrier();
    common->queue_enable = 1;

    return 0;
}

void vp_del_vq(struct vp_virtqueue *vq)
{
    if (!vq) { return; }

    free(vq->free_descs);
    free(vq->desc_data);
    free(vq->queue_mem);
    memset(vq, 0, sizeof(*vq));
}

void vp_notify(struct vp_virtqueue *vq)
{
    (void)vq;
}

/* ------------------------------------------------------------------ */
/* Virtqueue submission helpers                                        */
/* ------------------------------------------------------------------ */

int virtqueue_add(struct vp_virtqueue *vq, void *data, int len, int write)
{
    uint16_t head;

    if (vq->num_free < 1) { return -ENOSPC; }

    spin_lock(&vq->lock);

    head          = vq->free_head;
    vq->free_head = vq->free_descs[head];
    vq->num_free--;

    vq->desc[head].addr  = (uint64_t)(uintptr_t)virt_any_to_phys((uintptr_t)data);
    vq->desc[head].len   = len;
    vq->desc[head].flags = write ? VRING_DESC_F_WRITE : 0;
    vq->desc[head].next  = 0;

    vq->desc_data[head] = data;

    /* Update avail ring */
    vq->avail->ring[vq->avail_idx_shadow & (vq->num_max - 1)] = head;
    vq->avail_idx_shadow++;

    compiler_barrier();
    vq->avail->idx = vq->avail_idx_shadow;

    spin_unlock(&vq->lock);
    return 0;
}

int virtqueue_add_out_in(struct vp_virtqueue *vq, void *out_data, int out_len, void *in_data, int in_len)
{
    uint16_t head, out_desc, in_desc;

    if (vq->num_free < 2) { return -ENOSPC; }

    spin_lock(&vq->lock);

    head          = vq->free_head;
    out_desc      = head;
    vq->free_head = vq->free_descs[out_desc];
    in_desc       = vq->free_head;
    vq->free_head = vq->free_descs[in_desc];
    vq->num_free -= 2;

    /* out descriptor: driver → device (device reads) */
    vq->desc[out_desc].addr  = (uint64_t)(uintptr_t)virt_any_to_phys((uintptr_t)out_data);
    vq->desc[out_desc].len   = out_len;
    vq->desc[out_desc].flags = VRING_DESC_F_NEXT;
    vq->desc[out_desc].next  = in_desc;

    /* in descriptor: device → driver (device writes) */
    vq->desc[in_desc].addr  = (uint64_t)(uintptr_t)virt_any_to_phys((uintptr_t)in_data);
    vq->desc[in_desc].len   = in_len;
    vq->desc[in_desc].flags = VRING_DESC_F_WRITE;
    vq->desc[in_desc].next  = 0;

    vq->desc_data[out_desc] = out_data;
    vq->desc_data[in_desc]  = in_data;

    /* Update avail ring */
    vq->avail->ring[vq->avail_idx_shadow & (vq->num_max - 1)] = head;
    vq->avail_idx_shadow++;

    compiler_barrier();
    vq->avail->idx = vq->avail_idx_shadow;

    spin_unlock(&vq->lock);
    return 0;
}

void *virtqueue_get_buf(struct vp_virtqueue *vq, uint32_t *len)
{
    void    *data;
    uint16_t head;
    uint16_t idx;

    spin_lock(&vq->lock);

    idx = vq->used->idx;
    if (vq->used_idx == idx) {
        spin_unlock(&vq->lock);
        return NULL;
    }

    head = vq->used->ring[vq->used_idx & (vq->num_max - 1)].id;
    if (len) { *len = vq->used->ring[vq->used_idx & (vq->num_max - 1)].len; }
    vq->used_idx++;

    data = vq->desc_data[head];

    /* Put descriptors back on free list */
    do {
        uint16_t next        = vq->desc[head].flags & VRING_DESC_F_NEXT ? vq->desc[head].next : 0;
        vq->free_descs[head] = vq->free_head;
        vq->free_head        = head;
        vq->num_free++;
        if (vq->desc[head].flags & VRING_DESC_F_INDIRECT) { break; }
        head = next;
    } while (head);

    spin_unlock(&vq->lock);
    return data;
}

static inline void virtio_wmb(void)
{
    __asm__ volatile("sfence" ::: "memory");
}

void virtqueue_kick(struct vp_virtqueue *vq)
{
    struct vp_device *vp = vq->vp;
    uint32_t          off;

    if (!vp || !vp->notify_base) { return; }

    off = vq->notify_off * vp->notify_off_multiplier;

    /*
     * VirtIO spec §4.1.4.4: A write barrier is required before the MMIO
     * doorbell write to ensure descriptor/avail writes are visible to the
     * device. A second barrier after ensures the store reaches the fabric
     * before the driver starts polling the used ring.
     */
    virtio_wmb();
    *(volatile uint32_t *)((uintptr_t)vp->notify_base + off) = vq->index;
    virtio_wmb();
}

int virtqueue_enable_cb(struct vp_virtqueue *vq)
{
    vq->avail->flags &= ~((volatile uint16_t)1);
    compiler_barrier();
    return (vq->used->idx != vq->used_idx);
}

/* ------------------------------------------------------------------ */
/* Device discovery and lifecycle                                      */
/* ------------------------------------------------------------------ */

int vp_find_device(uint16_t vendor_id, uint16_t device_id, struct vp_device *dev)
{
    pci_device_cache_t  *cache;
    pci_device_request_t req;
    int                  ret;

    memset(dev, 0, sizeof(*dev));

    req.vendor_id = vendor_id;
    req.device_id = device_id;

    /* Use the project's PCI device finding API */
    cache = pci_found_device_cache(NULL, req);
    if (!cache) {
        /* Force a re-scan of the PCI bus */
        pci_flush_devices_cache();
        cache = pci_found_device_cache(NULL, req);
        if (!cache) { return -ENODEV; }
    }

    dev->pci_dev   = cache;
    dev->vendor_id = vendor_id;
    dev->device_id = device_id;

    VP_DBG("Found device %04x:%04x at %04x:%02x:%02x.%01x\n", vendor_id, device_id, cache->device->domain, cache->device->bus,
           cache->device->slot, cache->device->func);

    /* Scan capabilities */
    ret = vp_scan_caps(dev);
    if (ret) { return ret; }

    /* Reset device */
    vp_reset_device(dev);

    return 0;
}

int vp_setup_device(struct vp_device *dev)
{
    /* Set ACKNOWLEDGE + DRIVER status */
    vp_set_status(dev, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER);
    return 0;
}

void vp_release_device(struct vp_device *dev)
{
    if (!dev) { return; }

    vp_reset_device(dev);
    dev->common      = NULL;
    dev->isr         = NULL;
    dev->device_cfg  = NULL;
    dev->notify_base = NULL;
    dev->pci_dev     = NULL;
}

/* ------------------------------------------------------------------ */
/* Module entry point — called by the virtio-gpu driver init           */
/* ------------------------------------------------------------------ */

/* No standalone init/exit here; the GPU driver calls vp_find_device */
