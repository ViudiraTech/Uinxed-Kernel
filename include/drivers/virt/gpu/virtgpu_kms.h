/*
 *
 *      virtgpu_kms.h
 *      VirtIO-GPU KMS display pipeline helpers
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_KMS_H_
#define INCLUDE_VIRTGPU_KMS_H_

#include <drivers/virt/gpu/virtgpu_drv.h>

/* Maximum number of scanouts supported */
#define VIRTGPU_MAX_SCANOUTS 16

/* Stride alignment for dumb buffers */
#define VIRTGPU_STRIDE_ALIGN 64

/* Default modes (used when host has no display info) */
#define VIRTGPU_DEFAULT_WIDTH  1024
#define VIRTGPU_DEFAULT_HEIGHT 768

#endif
