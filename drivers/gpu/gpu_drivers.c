/*
 *
 *      gpu_drivers.c
 *      Built-in GPU driver table
 *
 *      2026/8/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/gpu_drivers.h>

void gpu_drivers_init(void)
{
#if CONFIG_VIRTIO_GPU
    drm_gpu_driver_register("virtio-gpu", virtio_gpu_probe);
    /* Future GPU drivers register their probe here. */
#endif
}
