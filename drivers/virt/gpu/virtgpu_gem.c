/*
 *
 *      virtgpu_gem.c
 *      VirtIO-GPU GEM object management
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Implements GEM object allocation, dumb buffer helpers, and PRIME
 *  export/import stubs for the virtio-gpu driver.
 *
 */

#include <drivers/virt/virtgpu_drv.h>
#include <drivers/virt/virtgpu_gem.h>
#include <drivers/virt/virtgpu_kms.h>
#include <kernel/printk.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <mem/hhdm.h>
#include <mem/page.h>

/* ------------------------------------------------------------------ */
/* Object alloc / free                                                 */
/* ------------------------------------------------------------------ */

struct virtio_gpu_object *virtgpu_gem_alloc_object(struct drm_device *dev, size_t size)
{
    struct virtio_gpu_object *obj;

    obj = malloc(sizeof(*obj));
    if (!obj) { return NULL; }
    memset(obj, 0, sizeof(*obj));
    obj->context_lock.lock = 0;

    /* Initialise the embedded GEM object */
    drm_gem_object_init(dev, &obj->base, size);

    /*
     * Virtio DMA descriptors carry physical ranges.  Allocate the resource
     * from the frame allocator so it is page-aligned and physically
     * contiguous; ordinary heap allocations may span unrelated frames and
     * must never be advertised to the host as one DMA segment.
     */
    if (size > 0) {
        if (size > UINT32_MAX || size > SIZE_MAX - (PAGE_4K_SIZE - 1)) {
            free(obj);
            return NULL;
        }
        obj->backing_page_count = ALIGN_UP(size, PAGE_4K_SIZE) / PAGE_4K_SIZE;
        obj->backing_phys       = alloc_frames(obj->backing_page_count);
        if (!obj->backing_phys) {
            plogk("virtgpu: GEM backing frame allocation failed (%lu pages)\n", (unsigned long)obj->backing_page_count);
            free(obj);
            return NULL;
        }
        obj->base.backing = phys_to_virt(obj->backing_phys);
        memset(obj->base.backing, 0, obj->backing_page_count * PAGE_4K_SIZE);

        /* Set up a single memory entry for virtio-gpu backing */
        obj->num_entries = 1;
        obj->entries     = malloc(sizeof(struct virtio_gpu_mem_entry));
        if (!obj->entries) {
            plogk("virtgpu: GEM memory entry allocation failed (size=%lu)\n", (unsigned long)size);
            free_frames(obj->backing_phys, obj->backing_page_count);
            free(obj);
            return NULL;
        }
        obj->entries[0].addr    = obj->backing_phys;
        obj->entries[0].length  = (uint32_t)size;
        obj->entries[0].padding = 0;
    }

    if (size && drm_gem_create_mmap_offset(&obj->base)) {
        free(obj->entries);
        free_frames(obj->backing_phys, obj->backing_page_count);
        free(obj);
        return NULL;
    }

    return obj;
}

void virtgpu_gem_free_object(struct drm_gem_object *gem_obj)
{
    struct virtio_gpu_object *obj = to_virtio_gpu_object(gem_obj);
    struct virtio_gpu_device *vgdev;

    if (!obj) { return; }
    vgdev = (struct virtio_gpu_device *)gem_obj->dev->dev_private;

    /* Release host-side resource */
    if (obj->hw_res_handle) {
        while (obj->context_attachments) {
            uint32_t ctx_id = obj->context_attachments->ctx_id;
            if (virtgpu_object_detach_context(vgdev, obj, ctx_id)) {
                struct virtio_gpu_context_attachment *stale = obj->context_attachments;
                obj->context_attachments                    = stale->next;
                free(stale);
            }
        }
        if (obj->backing_attached) virtgpu_cmd_detach_backing(vgdev, obj->hw_res_handle);
        virtgpu_cmd_unref_resource(vgdev, obj->hw_res_handle);
    }

    free(obj->entries);
    if (obj->backing_phys) free_frames(obj->backing_phys, obj->backing_page_count);
    obj->base.backing = NULL;
    free(obj);
}

/* ------------------------------------------------------------------ */
/* Dumb buffer helpers                                                 */
/* ------------------------------------------------------------------ */

int virtgpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev, struct drm_mode_create_dumb *args)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct virtio_gpu_object *obj;
    size_t                    size;
    int                       ret;
    uint32_t                  handle;

    if (!args || !args->width || !args->height || args->bpp != 32 || args->flags) return -EINVAL;
    if (args->width > dev->mode_config.max_width || args->height > dev->mode_config.max_height) return -EINVAL;
    if (args->width > UINT32_MAX / 4) return -EINVAL;

    /* Round up pitch to alignment */
    args->pitch = ALIGN_UP(args->width * (args->bpp / 8), VIRTGPU_STRIDE_ALIGN);
    args->size  = (uint64_t)args->pitch * args->height;
    size        = args->size;

    obj = virtgpu_gem_alloc_object(dev, size);
    if (!obj) { return -ENOMEM; }

    obj->format     = DRM_FORMAT_XRGB8888;
    obj->width      = args->width;
    obj->height     = args->height;
    obj->stride     = args->pitch;
    obj->depth      = 24;
    obj->created_3d = false;

    /* Allocate a host-side resource ID */
    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);

    /* Create 2D resource on host */
    ret = virtgpu_cmd_create_resource_2d(vgdev, obj);
    if (ret) {
        obj->hw_res_handle = 0;
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    /* Attach backing storage */
    ret = virtgpu_cmd_attach_backing(vgdev, obj);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }
    obj->backing_attached = true;

    /* Create GEM handle for userspace */
    ret = drm_gem_handle_create(file_priv, &obj->base, &handle);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    args->handle = handle;
    drm_gem_object_put(&obj->base);
    DRM_DEBUG_DRIVER("Dumb buffer created: %ux%u, pitch=%u, size=%llu, handle=%u\n", args->width, args->height, args->pitch, args->size, handle);
    return 0;
}

int virtgpu_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle, uint64_t *offset)
{
    struct drm_gem_object *gem_obj;

    (void)dev;
    gem_obj = drm_gem_object_lookup(file_priv, handle);
    if (!gem_obj) { return -ENOENT; }

    /* Use the GEM object's backing memory address as the mmap offset.
     * The core DRM mmap handler (drm_dev_mmap) will look it up. */
    if (!gem_obj->mmap_offset) {
        drm_gem_object_put(gem_obj);
        return -EINVAL;
    }
    *offset = gem_obj->mmap_offset;

    drm_gem_object_put(gem_obj);
    return 0;
}

/* ------------------------------------------------------------------ */
/* PRIME export / import (dma-buf)                                     */
/* ------------------------------------------------------------------ */

int virtgpu_gem_prime_export(struct drm_device *dev, struct drm_gem_object *obj, int *prime_fd)
{
    (void)obj;
    return drm_gem_prime_handle_to_fd(dev, NULL, 0, DRM_PRIME_CAP_EXPORT, prime_fd);
}

struct drm_gem_object *virtgpu_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    if (!obj || obj->dev != dev) { return NULL; }
    drm_gem_object_get(obj);
    return obj;
}
