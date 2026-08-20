/*
 *
 *      virtgpu_format.h
 *      VirtIO-GPU scanout format compatibility helpers
 *
 *      2026/8/20 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_FORMAT_H_
#define INCLUDE_VIRTGPU_FORMAT_H_

#include <drivers/gpu/drm/drm_fourcc.h>
#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

/*
 * XRGB8888 and ARGB8888 have the same byte and colour-channel layout; only
 * the meaning of the high byte differs.  A final scanout is opaque, so a 2D
 * XRGB resource can safely back an ARGB framebuffer view (and vice versa).
 */
static inline bool virtgpu_2d_formats_compatible(uint32_t resource_format, uint32_t framebuffer_format)
{
    if (resource_format == framebuffer_format) return true;

    return (resource_format == DRM_FORMAT_XRGB8888 && framebuffer_format == DRM_FORMAT_ARGB8888) || (resource_format == DRM_FORMAT_ARGB8888 && framebuffer_format == DRM_FORMAT_XRGB8888);
}

#endif // INCLUDE_VIRTGPU_FORMAT_H_
