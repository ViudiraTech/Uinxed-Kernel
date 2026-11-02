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

/*
 * The bus owns driver discovery, keeping the DRM core free of any knowledge
 * about specific GPU drivers.  Each driver only exposes a probe function
 * through its own public header and attaches via the DRM device API.
 */

static struct gpu_driver {
        const char *name;
        int (*probe)(bool *display_owned);
        bool fallback;
} gpu_drivers[CONFIG_GPU_MAX_GPU_DRIVERS];

static int gpu_driver_count;

#if CONFIG_DRM && (CONFIG_VIRTIO_GPU || CONFIG_BOCHS || CONFIG_SIMPLEDRM)

static spinlock_t gpu_driver_lock = {.lock = 0, .rflags = 0};

/* Register a built-in GPU driver probe callback with the bus. */
static void gpu_driver_register(const char *name, int (*probe)(bool *display_owned), bool fallback)
{
    if (!name || !probe) return;

    spin_lock(&gpu_driver_lock);
    if (gpu_driver_count >= CONFIG_GPU_MAX_GPU_DRIVERS) {
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

#endif

/* Probe every registered GPU driver, hardware first and framebuffers last. */
int gpu_drivers_probe(void)
{
    bool display_owned = false;
    int  attached      = 0;

    /* Real GPU drivers first; a working accelerator wins over the framebuffer. */
    for (int i = 0; i < gpu_driver_count; i++) {
        bool owned = false;
        int  ret;
        if (gpu_drivers[i].fallback || !gpu_drivers[i].probe) continue;

        ret = gpu_drivers[i].probe(&owned);
        if (ret < 0) continue;

        attached++;
        plogk("gpu: GPU driver \"%s\" attached.\n", gpu_drivers[i].name);

        if (owned) {
            /* The console handoff is global; stop before a second display takes it over. */
            display_owned = true;
            break;
        }
        plogk("gpu: GPU driver \"%s\" has no display, continuing.\n", gpu_drivers[i].name);
    }
    if (display_owned) return 0;

    /* No driver took the display: probe the software framebuffer DRM drivers. */
    for (int i = 0; i < gpu_driver_count; i++) {
        bool owned = false;
        if (!gpu_drivers[i].fallback || !gpu_drivers[i].probe) continue;
        if (gpu_drivers[i].probe(&owned) == 0) {
            attached++;
            plogk("gpu: GPU driver \"%s\" attached.\n", gpu_drivers[i].name);
            if (owned) break;
        }
    }

    /* No DRM driver at all: the console stays on the plain boot framebuffer. */
    if (!attached) plogk("gpu: No GPU driver available, console remains on the boot framebuffer.\n");
    return attached ? 0 : -ENODEV;
}

/* Register every built-in GPU driver with the bus. */
void gpu_drivers_init(void)
{
#if CONFIG_VIRTIO_GPU && CONFIG_DRM && CONFIG_VIRTIO_PCI
    gpu_driver_register("virtio_gpu", virtio_gpu_probe, false);
#endif
#if CONFIG_SIMPLEDRM && CONFIG_DRM
    gpu_driver_register("simpledrm", simpledrm_probe, true);
#endif
}
