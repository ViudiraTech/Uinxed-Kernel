/*
 *
 *      drm_color_mgmt.h
 *      DRM color management UAPI
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux DRM UAPI (include/uapi/drm/drm_color_mgmt.h).
 *
 */

#ifndef INCLUDE_DRM_DRM_COLOR_MGMT_H_
#define INCLUDE_DRM_DRM_COLOR_MGMT_H_

#include <drm/drm.h>

/* DRM_MODE_PROPERTY_BLOB for degamma/gamma LUT */
#define DRM_MODE_LUT_GAMMA  (1 << 0)
#define DRM_MODE_LUT_DEGAMMA (1 << 1)

struct drm_color_lut {
        __u16 red;
        __u16 green;
        __u16 blue;
        __u16 reserved;
};

struct drm_color_ctm {
        __s64 matrix[9]; // 4.32 fixed-point signed matrix
};

struct drm_color_lut_range {
        __u16 ramp_size;
        __u16 lut_size;
        __u32 properties;
        __u32 start;
        __u32 end;
};

#endif /* INCLUDE_DRM_DRM_COLOR_MGMT_H_ */
