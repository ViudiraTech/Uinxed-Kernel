/*
 *
 *      virtgpu_vq.h
 *      VirtIO-GPU virtqueue helpers
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_VQ_H_
#define INCLUDE_VIRTGPU_VQ_H_

#include <drivers/virt/gpu/virtgpu_drv.h>

/* Number of descriptors per virtqueue */
#define VIRTGPU_VQ_NUM 256

/* Queue indices */
#define VIRTGPU_CTRLQ  0
#define VIRTGPU_CURSORQ 1

#endif
