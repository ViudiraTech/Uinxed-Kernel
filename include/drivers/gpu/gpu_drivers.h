/*
 *
 *      gpu_drivers.h
 *      Built-in GPU driver bus
 *
 *      2026/8/18 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRIVERS_GPU_DRIVERS_H_
#define INCLUDE_DRIVERS_GPU_DRIVERS_H_

/*
 * Probe every registered GPU driver: real hardware drivers first, and only
 * if none attach, the framebuffer fallback drivers.  Returns 0 if at least
 * one driver attached, otherwise -ENODEV.
 */
int gpu_drivers_probe(void);

/* Register every built-in GPU driver with the bus. */
void gpu_drivers_init(void);

#endif // INCLUDE_DRIVERS_GPU_DRIVERS_H_
