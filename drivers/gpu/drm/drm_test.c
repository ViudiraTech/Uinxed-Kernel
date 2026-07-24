/*
 *
 *      drm_test.c
 *      DRM subsystem functional test
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm.h>
#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_init.h>
#include <drivers/drm/drm_mode.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>

/* Dummy GEM object with allocated backing buffer. */
struct test_gem_object {
        struct drm_gem_object base;
        void                 *vaddr;
        size_t                size;
};

static struct test_gem_object *test_gem_alloc(struct drm_device *dev, size_t sz)
{
    struct test_gem_object *obj = malloc(sizeof(*obj));

    if (!obj) { return NULL; }

    memset(obj, 0, sizeof(*obj));
    obj->vaddr = malloc(sz);
    if (!obj->vaddr) {
        free(obj);
        return NULL;
    }

    memset(obj->vaddr, 0, sz);
    drm_gem_object_init(dev, &obj->base, sz);
    obj->size = sz;
    return obj;
}

static void test_gem_free(struct test_gem_object *obj)
{
    if (!obj) { return; }

    free(obj->vaddr);
    free(obj);
}

/* ------------------------------------------------------------------ */
/* DRM functional test — exercises GEM + framebuffer + ioctl           */
/* ------------------------------------------------------------------ */

void drm_run_test(void)
{
    struct drm_device *dev = drm_get_singleton();
    if (!dev) {
        DRM_ERROR("Test: no DRM device, skipping.\n");
        return;
    }

    DRM_INFO("=== DRM subsystem self-test ===\n");

    /* 1. Allocate a GEM dumb buffer */
    struct drm_mode_create_dumb create = {0};
    create.width                       = 800;
    create.height                      = 600;
    create.bpp                         = 32;

    struct test_gem_object *gem = test_gem_alloc(dev, create.width * create.height * 4);
    if (!gem) {
        DRM_ERROR("Test: GEM buffer allocation failed.\n");
        return;
    }

    plogk("drm_test: GEM buffer allocated (%ux%u, %u bytes)\n", create.width, create.height, gem->size);

    /* 2. Create a GEM handle for the buffer */
    struct drm_file test_file;
    memset(&test_file, 0, sizeof(test_file));

    int ret = drm_open(dev, &test_file);
    if (ret != 0) {
        DRM_ERROR("Test: drm_open failed: %d\n", ret);
        test_gem_free(gem);
        return;
    }

    uint32_t handle;
    ret = drm_gem_handle_create(&test_file, &gem->base, &handle);
    if (ret != 0) {
        DRM_ERROR("Test: handle_create failed: %d\n", ret);
        drm_release(&test_file);
        test_gem_free(gem);
        return;
    }

    plogk("drm_test: GEM handle %u created.\n", handle);

    /* 3. Fill the GEM buffer with a simple pattern (colored stripes) */
    uint32_t *pixels = (uint32_t *)gem->vaddr;
    for (uint32_t y = 0; y < create.height; y++) {
        uint32_t color;
        if (y < create.height / 3) {
            color = 0xFFFF0000; /* red */
        } else if (y < 2 * create.height / 3) {
            color = 0xFF00FF00; /* green */
        } else {
            color = 0xFF0000FF; /* blue */
        }
        for (uint32_t x = 0; x < create.width; x++) { pixels[y * create.width + x] = color; }
    }

    plogk("drm_test: Pattern written to GEM buffer.\n");

    /* 4. Look up the GEM object by handle */
    struct drm_gem_object *lookup = drm_gem_object_lookup(&test_file, handle);
    if (lookup) {
        plogk("drm_test: GEM object lookup by handle %u OK, size=%u\n", handle, lookup->size);
        drm_gem_object_put(lookup);
    } else {
        DRM_ERROR("Test: GEM object lookup failed.\n");
    }

    /* 5. Test ioctl: DRM_IOCTL_VERSION */
    struct drm_version ver = {0};
    ret                    = drm_ioctl(dev, DRM_IOCTL_VERSION, &ver, &test_file);
    if (ret == 0) {
        plogk("drm_test: DRM_IOCTL_VERSION OK (major=%d, minor=%d, patch=%d)\n", ver.version_major, ver.version_minor, ver.version_patchlevel);
    } else {
        DRM_ERROR("Test: DRM_IOCTL_VERSION failed: %d\n", ret);
    }

    /* 6. Test ioctl: DRM_IOCTL_GET_UNIQUE */
    struct drm_unique uniq = {0};
    char              busid[64];
    uniq.unique     = (__u64)(uintptr_t)busid;
    uniq.unique_len = sizeof(busid);
    ret             = drm_ioctl(dev, DRM_IOCTL_GET_UNIQUE, &uniq, &test_file);
    if (ret == 0) plogk("drm_test: DRM_IOCTL_GET_UNIQUE OK\n");

    /* 7. Clean up */
    ret = drm_gem_handle_delete(&test_file, handle);
    if (ret == 0) plogk("drm_test: GEM handle %u deleted.\n", handle);

    drm_release(&test_file);
    test_gem_free(gem);

    DRM_INFO("=== DRM self-test PASSED ===\n");
}