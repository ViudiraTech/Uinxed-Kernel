/*
 *
 *      virtgpu_kms.h
 *      VirtIO-GPU KMS display pipeline helpers
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_KMS_H_
#define INCLUDE_VIRTGPU_KMS_H_

#include <drivers/gpu/drm/virtio/virtgpu_drv.h>

/* Maximum number of scanouts supported */
#define VIRTGPU_MAX_SCANOUTS 16

/*
 * Stride alignment for dumb buffers. RESOURCE_CREATE_2D has no stride field:
 * guest backing rows are tightly packed at width * bytes_per_pixel.
 */
#define VIRTGPU_STRIDE_ALIGN 4

#endif // INCLUDE_VIRTGPU_KMS_H_
