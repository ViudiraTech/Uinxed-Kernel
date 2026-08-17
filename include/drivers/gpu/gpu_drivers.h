/*
 *
 *      gpu_drivers.h
 *      Built-in GPU driver registration
 *
 *      2026/8/17 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_GPU_DRIVERS_H_
#define INCLUDE_DRIVERS_GPU_DRIVERS_H_

/*
 * Register every built-in GPU driver with the DRM framework.  Called once
 * from the boot path before drm_gpu_probe_all().  Adding a GPU driver means
 * adding one entry to the table in drivers/gpu/gpu_drivers.c.
 */
void gpu_drivers_init(void);

#endif // INCLUDE_DRIVERS_GPU_DRIVERS_H_
