/*
 *
 *      vmfgfx_gem.c
 *      VMware SVGA II display driver (vmfgfx) - GEM objects
 *
 *      2026/10/04 By Yinyuan34513
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/vmware/vmfgfx.h>
#include <kernel/errno.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

#if CONFIG_VMFGFX && CONFIG_DRM

/*
 * Allocate a GEM object for a framebuffer this driver will scan out itself.
 *
 * The device has no DMA path of its own: every refresh is a CPU copy out of
 * the object into video RAM, so system memory is exactly as good as any
 * other backing and mmap works without any extra plumbing.  The generic
 * dumb-buffer helpers hand out these objects to userspace, this one only
 * exists for the framebuffer the driver allocates on its own behalf.
 */
struct drm_gem_object *vmfgfx_gem_create(struct drm_device *dev, size_t size)
{
    struct drm_gem_object *obj;
    size_t                 rounded;

    if (!dev || !size || size > UINT32_MAX) return NULL;

    /* The console expects a page-aligned backing store. */
    rounded = ALIGN_UP(size, PAGE_4K_SIZE);

    obj = malloc(sizeof(*obj));
    if (!obj) return NULL;
    memset(obj, 0, sizeof(*obj));

    if (drm_gem_object_init(dev, obj, size)) {
        free(obj);
        return NULL;
    }

    obj->backing = aligned_alloc(4096, rounded);
    if (!obj->backing) {
        free(obj);
        return NULL;
    }
    memset(obj->backing, 0, rounded);
    obj->prime_fd = -1;

    if (drm_gem_create_mmap_offset(obj)) {
        free(obj->backing);
        free(obj);
        return NULL;
    }
    return obj;
}

/* Import a dma-buf that this device exported earlier. */
struct drm_gem_object *vmfgfx_gem_prime_import(struct drm_device *dev, void *dma_buf)
{
    struct drm_gem_object *obj = (struct drm_gem_object *)dma_buf;

    (void)dev;
    if (!obj) return NULL;
    drm_gem_object_get(obj);
    return obj;
}

#endif
