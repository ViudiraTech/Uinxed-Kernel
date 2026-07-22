/*
 *
 *      drm_fourcc.h
 *      DRM fourcc pixel format definitions
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux DRM UAPI (include/uapi/drm/drm_fourcc.h).
 *
 */

#ifndef INCLUDE_DRM_DRM_FOURCC_H_
#define INCLUDE_DRM_DRM_FOURCC_H_

#include <drm/drm.h>

#define fourcc_code(a, b, c, d) ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

#define DRM_FORMAT_BIG_ENDIAN 0x80000000U
#define DRM_FORMAT_INVALID    0

/* Colorspace / format helpers */
#define DRM_FORMAT_C8       fourcc_code('C', '8', ' ', ' ') // 8-bit indexed color
#define DRM_FORMAT_RGB332   fourcc_code('R', 'G', 'B', '8')
#define DRM_FORMAT_BGR233   fourcc_code('B', 'G', 'R', '8')
#define DRM_FORMAT_XRGB4444 fourcc_code('X', 'R', '1', '2')
#define DRM_FORMAT_XBGR4444 fourcc_code('X', 'B', '1', '2')
#define DRM_FORMAT_RGBX4444 fourcc_code('R', 'X', '1', '2')
#define DRM_FORMAT_BGRX4444 fourcc_code('B', 'X', '1', '2')
#define DRM_FORMAT_ARGB4444 fourcc_code('A', 'R', '1', '2')
#define DRM_FORMAT_ABGR4444 fourcc_code('A', 'B', '1', '2')
#define DRM_FORMAT_RGBA4444 fourcc_code('R', 'A', '1', '2')
#define DRM_FORMAT_BGRA4444 fourcc_code('B', 'A', '1', '2')
#define DRM_FORMAT_XRGB1555 fourcc_code('X', 'R', '1', '5')
#define DRM_FORMAT_XBGR1555 fourcc_code('X', 'B', '1', '5')
#define DRM_FORMAT_RGBX5551 fourcc_code('R', 'X', '1', '5')
#define DRM_FORMAT_BGRX5551 fourcc_code('B', 'X', '1', '5')
#define DRM_FORMAT_ARGB1555 fourcc_code('A', 'R', '1', '5')
#define DRM_FORMAT_ABGR1555 fourcc_code('A', 'B', '1', '5')
#define DRM_FORMAT_RGBA5551 fourcc_code('R', 'A', '1', '5')
#define DRM_FORMAT_BGRA5551 fourcc_code('B', 'A', '1', '5')
#define DRM_FORMAT_RGB565   fourcc_code('R', 'G', '1', '6')
#define DRM_FORMAT_BGR565   fourcc_code('B', 'G', '1', '6')
#define DRM_FORMAT_RGB888   fourcc_code('R', 'G', '2', '4')
#define DRM_FORMAT_BGR888   fourcc_code('B', 'G', '2', '4')
#define DRM_FORMAT_XRGB8888 fourcc_code('X', 'R', '2', '4')
#define DRM_FORMAT_XBGR8888 fourcc_code('X', 'B', '2', '4')
#define DRM_FORMAT_RGBX8888 fourcc_code('R', 'X', '2', '4')
#define DRM_FORMAT_BGRX8888 fourcc_code('B', 'X', '2', '4')
#define DRM_FORMAT_ARGB8888 fourcc_code('A', 'R', '2', '4')
#define DRM_FORMAT_ABGR8888 fourcc_code('A', 'B', '2', '4')
#define DRM_FORMAT_RGBA8888 fourcc_code('R', 'A', '2', '4')
#define DRM_FORMAT_BGRA8888 fourcc_code('B', 'A', '2', '4')
#define DRM_FORMAT_XRGB2101010 fourcc_code('X', 'R', '3', '0')
#define DRM_FORMAT_XBGR2101010 fourcc_code('X', 'B', '3', '0')
#define DRM_FORMAT_RGBX1010102 fourcc_code('R', 'X', '3', '0')
#define DRM_FORMAT_BGRX1010102 fourcc_code('B', 'X', '3', '0')
#define DRM_FORMAT_ARGB2101010 fourcc_code('A', 'R', '3', '0')
#define DRM_FORMAT_ABGR2101010 fourcc_code('A', 'B', '3', '0')
#define DRM_FORMAT_RGBA1010102 fourcc_code('R', 'A', '3', '0')
#define DRM_FORMAT_BGRA1010102 fourcc_code('B', 'A', '3', '0')

/* 16-bit per component */
#define DRM_FORMAT_XRGB16161616 fourcc_code('X', 'R', '4', '8')
#define DRM_FORMAT_XBGR16161616 fourcc_code('X', 'B', '4', '8')
#define DRM_FORMAT_ARGB16161616 fourcc_code('A', 'R', '4', '8')
#define DRM_FORMAT_ABGR16161616 fourcc_code('A', 'B', '4', '8')

/* Packed YUV formats */
#define DRM_FORMAT_YUYV       fourcc_code('Y', 'U', 'Y', 'V')
#define DRM_FORMAT_YVYU       fourcc_code('Y', 'V', 'Y', 'U')
#define DRM_FORMAT_UYVY       fourcc_code('U', 'Y', 'V', 'Y')
#define DRM_FORMAT_VYUY       fourcc_code('V', 'Y', 'U', 'Y')
#define DRM_FORMAT_AYUV       fourcc_code('A', 'Y', 'U', 'V')
#define DRM_FORMAT_XYUV8888   fourcc_code('X', 'Y', 'U', 'V')

/* 2-plane YUV */
#define DRM_FORMAT_NV12       fourcc_code('N', 'V', '1', '2')
#define DRM_FORMAT_NV21       fourcc_code('N', 'V', '2', '1')
#define DRM_FORMAT_NV16       fourcc_code('N', 'V', '1', '6')
#define DRM_FORMAT_NV61       fourcc_code('N', 'V', '6', '1')
#define DRM_FORMAT_NV24       fourcc_code('N', 'V', '2', '4')
#define DRM_FORMAT_NV42       fourcc_code('N', 'V', '4', '2')
#define DRM_FORMAT_P010       fourcc_code('P', '0', '1', '0')
#define DRM_FORMAT_P012       fourcc_code('P', '0', '1', '2')
#define DRM_FORMAT_P016       fourcc_code('P', '0', '1', '6')

/* 3-plane YUV */
#define DRM_FORMAT_YUV410     fourcc_code('Y', 'U', 'V', '9')
#define DRM_FORMAT_YVU410     fourcc_code('Y', 'V', 'U', '9')
#define DRM_FORMAT_YUV411     fourcc_code('Y', 'U', '1', '1')
#define DRM_FORMAT_YVU411     fourcc_code('Y', 'V', '1', '1')
#define DRM_FORMAT_YUV420     fourcc_code('Y', 'U', '1', '2')
#define DRM_FORMAT_YVU420     fourcc_code('Y', 'V', '1', '2')
#define DRM_FORMAT_YUV422     fourcc_code('Y', 'U', '1', '6')
#define DRM_FORMAT_YVU422     fourcc_code('Y', 'V', '1', '6')
#define DRM_FORMAT_YUV444     fourcc_code('Y', 'U', '2', '4')
#define DRM_FORMAT_YVU444     fourcc_code('Y', 'V', '2', '4')

/* Format modifiers */
#define DRM_FORMAT_MOD_INVALID 0x00ffffffffffffffULL
#define DRM_FORMAT_MOD_LINEAR  0ULL

#define DRM_FORMAT_MOD_VENDOR_NONE    0
#define DRM_FORMAT_MOD_VENDOR_INTEL   0x01
#define DRM_FORMAT_MOD_VENDOR_AMD     0x02
#define DRM_FORMAT_MOD_VENDOR_NVIDIA  0x03
#define DRM_FORMAT_MOD_VENDOR_SAMSUNG 0x04
#define DRM_FORMAT_MOD_VENDOR_QCOM    0x05
#define DRM_FORMAT_MOD_VENDOR_VIVANTE 0x06
#define DRM_FORMAT_MOD_VENDOR_BROADCOM 0x07
#define DRM_FORMAT_MOD_VENDOR_ARM     0x08
#define DRM_FORMAT_MOD_VENDOR_ALLWINNER 0x09
#define DRM_FORMAT_MOD_VENDOR_AMLOGIC 0x0a

#define DRM_FORMAT_MOD_VENDOR_NONE_ fourcc_code(' ', ' ', ' ', ' ')

#define fourcc_mod_code(vendor, val) ((((uint64_t)DRM_FORMAT_MOD_VENDOR_##vendor) << 56) | ((val) & 0x00ffffffffffffffULL))

#define I915_FORMAT_MOD_X_TILED    fourcc_mod_code(INTEL, 1)
#define I915_FORMAT_MOD_Y_TILED    fourcc_mod_code(INTEL, 2)
#define I915_FORMAT_MOD_Y_TILED_CCS fourcc_mod_code(INTEL, 4)
#define I915_FORMAT_MOD_Yf_TILED   fourcc_mod_code(INTEL, 3)

#define AFBC_FORMAT_MOD_BLOCK_SIZE_16x16     (1ULL)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_32x8      (2ULL)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_64x4      (3ULL)
#define AFBC_FORMAT_MOD_BLOCK_SIZE_32x8_64x4 (4ULL)

#endif /* INCLUDE_DRM_DRM_FOURCC_H_ */
