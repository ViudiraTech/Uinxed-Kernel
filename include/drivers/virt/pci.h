/*
 *
 *      pci.h
 *      VirtIO PCI transport layer header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Implements the virtio-over-PCI transport as defined by the OASIS
 *  Virtio-PCI spec (virtio 1.0+).  Provides capability-based MMIO
 *  discovery, common-config / notify / ISR / device-config access,
 *  virtqueue setup, and feature negotiation.  This is a "legacy-free"
 *  implementation — only modern (PCI capabilities) mode is supported.
 *
 */

#ifndef INCLUDE_VIRT_PCI_H_
#define INCLUDE_VIRT_PCI_H_

#include <drivers/pci.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <sync/spin_lock.h>

/* ------------------------------------------------------------------ */
/* VirtIO standard PCI vendor / device IDs                             */
/* ------------------------------------------------------------------ */

#define PCI_VENDOR_ID_REDHAT          0x1af4
#define PCI_DEVICE_ID_VIRTIO_BASE     0x1000
#define PCI_DEVICE_ID_VIRTIO_GPU      0x1050

/* ------------------------------------------------------------------ */
/* VirtIO PCI capability types (PCI SIG vendor-defined)                */
/* ------------------------------------------------------------------ */

#define VIRTIO_PCI_CAP_COMMON_CFG      1
#define VIRTIO_PCI_CAP_NOTIFY_CFG      2
#define VIRTIO_PCI_CAP_ISR_CFG         3
#define VIRTIO_PCI_CAP_DEVICE_CFG      4
#define VIRTIO_PCI_CAP_PCI_CFG         5
#define VIRTIO_PCI_CAP_SHARED_MEMORY_CFG 8
#define VIRTIO_PCI_CAP_ADMIN_CFG       9

/* ------------------------------------------------------------------ */
/* Device status flags (written to common->device_status)              */
/* ------------------------------------------------------------------ */

#define VIRTIO_STATUS_RESET        0
#define VIRTIO_STATUS_ACKNOWLEDGE  (1 << 0)
#define VIRTIO_STATUS_DRIVER       (1 << 1)
#define VIRTIO_STATUS_DRIVER_OK    (1 << 2)
#define VIRTIO_STATUS_FEATURES_OK  (1 << 3)
#define VIRTIO_STATUS_NEEDS_RESET  (1 << 6)
#define VIRTIO_STATUS_FAILED       (1 << 7)

/* ------------------------------------------------------------------ */
/* Virtqueue descriptor flags                                          */
/* ------------------------------------------------------------------ */

#define VRING_DESC_F_NEXT   1
#define VRING_DESC_F_WRITE  2
#define VRING_DESC_F_INDIRECT 4

/* ------------------------------------------------------------------ */
/* VirtIO PCI capability header (at BAR + offset, 8 bytes)             */
/* ------------------------------------------------------------------ */

struct vp_cap {
        uint8_t  cap_vndr;
        uint8_t  cap_next;
        uint8_t  cfg_type;
        uint8_t  bar;
        uint32_t offset;
        uint32_t length;
} __attribute__((packed));

/* Notification capability extends vp_cap with a multiplier field. */
struct vp_notify_cap {
        struct vp_cap cap;
        uint32_t      notify_off_multiplier;
} __attribute__((packed));

/* ------------------------------------------------------------------ */
/* Common configuration structure (MMIO view, at common->offset)       */
/* ------------------------------------------------------------------ */

struct vp_common_cfg {
        volatile uint32_t device_feature_select;
        volatile uint32_t device_feature;
        volatile uint32_t driver_feature_select;
        volatile uint32_t driver_feature;
        volatile uint16_t msix_config;
        volatile uint16_t num_queues;
        volatile uint8_t  device_status;
        volatile uint8_t  config_generation;
        volatile uint16_t queue_select;
        volatile uint16_t queue_size;
        volatile uint16_t queue_msix_vector;
        volatile uint16_t queue_enable;
        volatile uint16_t queue_notify_off;
        volatile uint64_t queue_desc;
        volatile uint64_t queue_avail;
        volatile uint64_t queue_used;
};

/* ------------------------------------------------------------------ */
/* Virtqueue ring structures (16-byte descriptor)                      */
/* ------------------------------------------------------------------ */

struct vring_desc {
        uint64_t addr;
        uint32_t len;
        uint16_t flags;
        uint16_t next;
};

struct vring_avail {
        volatile uint16_t flags;
        volatile uint16_t idx;
        volatile uint16_t ring[];
};

struct vring_used_elem {
        uint32_t id;
        uint32_t len;
};

struct vring_used {
        volatile uint16_t flags;
        volatile uint16_t idx;
        struct vring_used_elem ring[];
};

/* ------------------------------------------------------------------ */
/* Virtqueue instance                                                  */
/* ------------------------------------------------------------------ */

struct vp_virtqueue {
        int                index;
        int                num_max;
        int                num_free;
        uint16_t           free_head;
        uint16_t           avail_idx_shadow;
        uint16_t           used_idx;
        spinlock_t         lock;

        struct vring_desc  *desc;
        struct vring_avail *avail;
        struct vring_used  *used;
        void               *queue_mem;

        uint16_t          *free_descs;
        void             **desc_data;

        struct vp_device  *vp;
        uint16_t           notify_off;
};

/* ------------------------------------------------------------------ */
/* VirtIO PCI device instance                                           */
/* ------------------------------------------------------------------ */

struct vp_device {
        pci_device_cache_t *pci_dev;
        uint32_t            vendor_id;
        uint32_t            device_id;

        /* MMIO pointers (identity-mapped virtual addresses) */
        struct vp_common_cfg volatile *common;
        volatile uint8_t              *isr;
        volatile uint8_t              *device_cfg;
        volatile uint32_t             *notify_base;
        uint32_t                       notify_off_multiplier;

        /* Capability cache */
        struct vp_cap          common_cap;
        struct vp_cap          isr_cap;
        struct vp_cap          device_cap;
        struct vp_notify_cap   notify_cap;

        /* Negotiated features */
        uint64_t features;

        /* Virtqueues */
        struct vp_virtqueue *vqs;
        int                  num_vqs;

        /* Private data for subclass drivers */
        void *private_data;
};

/* ------------------------------------------------------------------ */
/* Transport API                                                       */
/* ------------------------------------------------------------------ */

int  vp_find_device(uint16_t vendor_id, uint16_t device_id, struct vp_device *dev);
void vp_release_device(struct vp_device *dev);
void vp_reset_device(struct vp_device *dev);
int  vp_setup_device(struct vp_device *dev);
int  vp_negotiate_features(struct vp_device *dev, uint64_t guest_features, uint64_t *negotiated);
void vp_set_status(struct vp_device *dev, uint8_t status);
uint8_t vp_get_status(struct vp_device *dev);
int  vp_setup_vq(struct vp_device *dev, int index, int num, struct vp_virtqueue *vq);
void vp_del_vq(struct vp_virtqueue *vq);
void vp_notify(struct vp_virtqueue *vq);
void vp_read_device_config(struct vp_device *dev, void *buf, int offset, int len);
void vp_write_device_config(struct vp_device *dev, const void *buf, int offset, int len);

/* ------------------------------------------------------------------ */
/* Virtqueue submission / completion helpers                            */
/* ------------------------------------------------------------------ */

int  virtqueue_add(struct vp_virtqueue *vq, void *data, int len, int write);
int  virtqueue_add_out_in(struct vp_virtqueue *vq, void *out_data, int out_len, void *in_data, int in_len);
void *virtqueue_get_buf(struct vp_virtqueue *vq, uint32_t *len);
void virtqueue_kick(struct vp_virtqueue *vq);
int  virtqueue_enable_cb(struct vp_virtqueue *vq);

#endif /* INCLUDE_VIRT_PCI_H_ */
