/*
 *
 *      simpledrm.h
 *      Simple software-framebuffer DRM driver (Limine GOP)
 *
 *      2026/8/18 By MicroFish
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_SIMPLEDRM_H_
#define INCLUDE_SIMPLEDRM_H_

/*
 * Probe the bootloader-provided GOP framebuffer and, if no other DRM device
 * has attached yet, register a KMS device that scans out userspace
 * framebuffers by copying them to the physical framebuffer (software
 * scanout).  Returns 0 when a device was attached, -ENODEV otherwise.
 */
int simpledrm_probe(void);

#endif // INCLUDE_SIMPLEDRM_H_
