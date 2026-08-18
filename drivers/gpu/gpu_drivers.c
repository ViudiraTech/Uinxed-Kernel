/*
 *
 *      gpu_drivers.c
 *      Built-in GPU driver bus
 *
 *      2026/8/18 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/simpledrm/simpledrm.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/gpu_drivers.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <sync/spin_lock.h>

/*
 * The bus owns driver discovery, keeping the DRM core free of any knowledge
 * about specific GPU drivers.  Each driver only exposes a probe function
 * through its own public header and attaches via the DRM device API.
 */

#define GPU_MAX_GPU_DRIVERS 16

static struct gpu_driver {
    const char *name;
    int (*probe)(void);
    bool fallback;
} gpu_drivers[GPU_MAX_GPU_DRIVERS];

static int        gpu_driver_count;
static spinlock_t gpu_driver_lock = {.lock = 0, .rflags = 0};

/* Register a built-in GPU driver probe callback with the bus. */
static void gpu_driver_register(const char *name, int (*probe)(void), bool fallback)
{
    if (!name || !probe) return;

    spin_lock(&gpu_driver_lock);
    if (gpu_driver_count >= GPU_MAX_GPU_DRIVERS) {
        spin_unlock(&gpu_driver_lock);
        plogk("gpu: Driver registry full, ignoring \"%s\"\n", name);
        return;
    }
    gpu_drivers[gpu_driver_count].name     = name;
    gpu_drivers[gpu_driver_count].probe    = probe;
    gpu_drivers[gpu_driver_count].fallback = fallback;
    gpu_driver_count++;
    spin_unlock(&gpu_driver_lock);
}

/* Probe every registered GPU driver, hardware first and framebuffers last. */
int gpu_drivers_probe(void)
{
    int attached = 0;

    /* Real GPU drivers first; a working accelerator wins over the framebuffer. */
    for (int i = 0; i < gpu_driver_count; i++) {
        if (gpu_drivers[i].fallback || !gpu_drivers[i].probe) continue;
        if (gpu_drivers[i].probe() == 0) {
            attached++;
            plogk("gpu: GPU driver \"%s\" attached.\n", gpu_drivers[i].name);
        }
    }
    if (attached) return 0;

    /* No real GPU: probe the software framebuffer DRM drivers. */
    for (int i = 0; i < gpu_driver_count; i++) {
        if (!gpu_drivers[i].fallback || !gpu_drivers[i].probe) continue;
        if (gpu_drivers[i].probe() == 0) {
            attached++;
            plogk("gpu: GPU driver \"%s\" attached.\n", gpu_drivers[i].name);
        }
    }

    /* No DRM driver at all: the console stays on the plain boot framebuffer. */
    if (!attached) plogk("gpu: No GPU driver available, console remains on the boot framebuffer.\n");
    return attached ? 0 : -ENODEV;
}

/* Register every built-in GPU driver with the bus. */
void gpu_drivers_init(void)
{
#if CONFIG_VIRTIO_GPU
    gpu_driver_register("virtio_gpu", virtio_gpu_probe, false);
#endif
#if CONFIG_SIMPLEDRM
    gpu_driver_register("simpledrm", simpledrm_probe, true);
#endif
}
