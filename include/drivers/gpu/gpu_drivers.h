/*
 *
 *      gpu_drivers.h
 *      Built-in GPU driver bus
 *
 *      2026/8/18 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_GPU_DRIVERS_H_
#define INCLUDE_GPU_DRIVERS_H_

/* Probe the hardware drivers, falling back to the framebuffer; 0 if attached, else -ENODEV. */
int gpu_drivers_probe(void);

/* Register every built-in GPU driver with the bus. */
void gpu_drivers_init(void);

#endif // INCLUDE_GPU_DRIVERS_H_
