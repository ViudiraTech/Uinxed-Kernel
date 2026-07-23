/*
 *
 *      virtgpu_gem.h
 *      VirtIO-GPU GEM object management helpers
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_GEM_H_
#define INCLUDE_VIRTGPU_GEM_H_

#include <drivers/virt/gpu/virtgpu_drv.h>

/* Helper to convert drm_gem_object to virtio_gpu_object */
#define to_virtio_gpu_object(obj) ((struct virtio_gpu_object *)(obj))

#endif
