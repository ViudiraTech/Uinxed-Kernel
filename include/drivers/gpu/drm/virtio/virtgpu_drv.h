/*
 *
 *      virtgpu_drv.h
 *      VirtIO-GPU DRM driver - main header
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_VIRTGPU_DRV_H_
#define INCLUDE_VIRTGPU_DRV_H_

#include <drivers/bus/pci.h>
#include <drivers/bus/virtpci.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* VirtIO GPU feature bits */

#define VIRTIO_GPU_F_VIRGL         0
#define VIRTIO_GPU_F_EDID          1
#define VIRTIO_GPU_F_RESOURCE_UUID 2
#define VIRTIO_GPU_F_RESOURCE_BLOB 3
#define VIRTIO_GPU_F_CONTEXT_INIT  4

/* VirtIO GPU protocol: command types (8-bit) */

enum virtio_gpu_ctrl_type {
    /* 2D commands */
    VIRTIO_GPU_CMD_GET_DISPLAY_INFO        = 0x0100,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_2D      = 0x0101,
    VIRTIO_GPU_CMD_RESOURCE_UNREF          = 0x0102,
    VIRTIO_GPU_CMD_SET_SCANOUT             = 0x0103,
    VIRTIO_GPU_CMD_RESOURCE_FLUSH          = 0x0104,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D     = 0x0105,
    VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING = 0x0106,
    VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING = 0x0107,
    VIRTIO_GPU_CMD_GET_CAPSET_INFO         = 0x0108,
    VIRTIO_GPU_CMD_GET_CAPSET              = 0x0109,
    VIRTIO_GPU_CMD_GET_EDID                = 0x010a,
    VIRTIO_GPU_CMD_RESOURCE_ASSIGN_UUID    = 0x010b,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB    = 0x010c,
    VIRTIO_GPU_CMD_SET_SCANOUT_BLOB        = 0x010d,

    /* 3D (virgl) commands */
    VIRTIO_GPU_CMD_CTX_CREATE            = 0x0200,
    VIRTIO_GPU_CMD_CTX_DESTROY           = 0x0201,
    VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE   = 0x0202,
    VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE   = 0x0203,
    VIRTIO_GPU_CMD_RESOURCE_CREATE_3D    = 0x0204,
    VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D   = 0x0205,
    VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D = 0x0206,
    VIRTIO_GPU_CMD_SUBMIT_3D             = 0x0207,
    VIRTIO_GPU_CMD_RESOURCE_MAP_BLOB     = 0x0208,
    VIRTIO_GPU_CMD_RESOURCE_UNMAP_BLOB   = 0x0209,

    VIRTIO_GPU_CMD_UPDATE_CURSOR = 0x0300,
    VIRTIO_GPU_CMD_MOVE_CURSOR   = 0x0301,

    /* Responses */
    VIRTIO_GPU_RESP_OK_NODATA               = 0x1100,
    VIRTIO_GPU_RESP_OK_DISPLAY_INFO         = 0x1101,
    VIRTIO_GPU_RESP_OK_CAPSET_INFO          = 0x1102,
    VIRTIO_GPU_RESP_OK_CAPSET               = 0x1103,
    VIRTIO_GPU_RESP_OK_EDID                 = 0x1104,
    VIRTIO_GPU_RESP_OK_RESOURCE_UUID        = 0x1105,
    VIRTIO_GPU_RESP_OK_MAP_INFO             = 0x1106,
    VIRTIO_GPU_RESP_ERR_UNSPEC              = 0x1200,
    VIRTIO_GPU_RESP_ERR_OUT_OF_MEMORY       = 0x1201,
    VIRTIO_GPU_RESP_ERR_INVALID_SCANOUT_ID  = 0x1202,
    VIRTIO_GPU_RESP_ERR_INVALID_RESOURCE_ID = 0x1203,
    VIRTIO_GPU_RESP_ERR_INVALID_CONTEXT_ID  = 0x1204,
    VIRTIO_GPU_RESP_ERR_INVALID_PARAMETER   = 0x1205,
};

/* VirtIO GPU pixel formats (byte-order in memory, little-endian) */

#define VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM 1
#define VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM 2
#define VIRTIO_GPU_FORMAT_A8R8G8B8_UNORM 3
#define VIRTIO_GPU_FORMAT_X8R8G8B8_UNORM 4
#define VIRTIO_GPU_FORMAT_B5G6R5_UNORM   7
#define VIRTIO_GPU_FORMAT_R8G8B8A8_UNORM 67
#define VIRTIO_GPU_FORMAT_X8B8G8R8_UNORM 68
#define VIRTIO_GPU_FORMAT_A8B8G8R8_UNORM 121
#define VIRTIO_GPU_FORMAT_R8G8B8X8_UNORM 134

#define VIRTIO_GPU_FLAG_FENCE         (1U << 0)
#define VIRTIO_GPU_FLAG_INFO_RING_IDX (1U << 1)

/* VirtIO GPU protocol structures (wire format, little-endian) */

struct virtio_gpu_ctrl_hdr {
        uint32_t type;
        uint32_t flags;
        uint64_t fence_id;
        uint32_t ctx_id;
        uint8_t  ring_idx;
        uint8_t  padding[3];
};

struct virtio_gpu_config {
        uint32_t events_read;
        uint32_t events_clear;
        uint32_t num_scanouts;
        uint32_t num_capsets;
};

/*
 * One synchronous control-queue request.  Multiple requests can be
 * published together and completed after a single device notification.
 */
struct virtgpu_vq_command {
        void *cmd;
        int   cmd_size;
        void *resp;
        int   resp_size;
};

struct virtio_gpu_rect {
        uint32_t x;
        uint32_t y;
        uint32_t width;
        uint32_t height;
};

struct virtio_gpu_box {
        uint32_t x;
        uint32_t y;
        uint32_t z;
        uint32_t w;
        uint32_t h;
        uint32_t d;
};

struct virtio_gpu_display_one {
        struct virtio_gpu_rect r;
        uint32_t               enabled;
        uint32_t               flags;
};

struct virtio_gpu_resp_display_info {
        struct virtio_gpu_ctrl_hdr    hdr;
        struct virtio_gpu_display_one pmodes[16];
};

struct virtio_gpu_create_resource_2d {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   format;
        uint32_t                   width;
        uint32_t                   height;
};

struct virtio_gpu_unref {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   padding;
};

struct virtio_gpu_set_scanout {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_gpu_rect     r;
        uint32_t                   scanout_id;
        uint32_t                   resource_id;
};

struct virtio_gpu_resource_flush {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_gpu_rect     r;
        uint32_t                   resource_id;
        uint32_t                   padding;
};

struct virtio_gpu_transfer_to_host_2d {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_gpu_rect     r;
        uint64_t                   offset;
        uint32_t                   resource_id;
        uint32_t                   padding;
};

struct virtio_gpu_mem_entry {
        uint64_t addr;
        uint32_t length;
        uint32_t padding;
};

struct virtio_gpu_resource_attach_backing {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   nr_entries;
};

struct virtio_gpu_get_capset_info {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   capset_index;
        uint32_t                   padding;
};

struct virtio_gpu_resp_capset_info {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   capset_id;
        uint32_t                   capset_max_version;
        uint32_t                   capset_max_size;
        uint32_t                   padding;
};

struct virtio_gpu_get_capset {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   capset_id;
        uint32_t                   capset_version;
};

struct virtio_gpu_resp_capset {
        struct virtio_gpu_ctrl_hdr hdr;
        uint8_t                    capset_data[];
};

struct virtio_gpu_get_edid {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   scanout_id;
        uint32_t                   padding;
};

struct virtio_gpu_resp_edid {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   size;
        uint32_t                   padding;
        uint8_t                    edid[1024];
};

/* 3D commands */

struct virtio_gpu_ctx_create {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   nlen;
        uint32_t                   context_init;
        char                       debug_name[64];
};

struct virtio_gpu_ctx_destroy {
        struct virtio_gpu_ctrl_hdr hdr;
};

struct virtio_gpu_set_scanout_blob {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_gpu_rect     r;
        uint32_t                   scanout_id;
        uint32_t                   resource_id;
        uint32_t                   width;
        uint32_t                   height;
        uint32_t                   format;
        uint32_t                   padding;
        uint32_t                   strides[4];
        uint32_t                   offsets[4];
};

struct virtio_gpu_cursor_pos {
        uint32_t scanout_id;
        uint32_t x;
        uint32_t y;
        uint32_t padding;
};

struct virtio_gpu_update_cursor {
        struct virtio_gpu_ctrl_hdr   hdr;
        struct virtio_gpu_cursor_pos pos;
        uint32_t                     resource_id;
        uint32_t                     hot_x;
        uint32_t                     hot_y;
        uint32_t                     padding;
};

struct virtio_gpu_ctx_resource {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   padding;
};

struct virtio_gpu_resource_create_3d {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   target;
        uint32_t                   format;
        uint32_t                   bind;
        uint32_t                   width;
        uint32_t                   height;
        uint32_t                   depth;
        uint32_t                   array_size;
        uint32_t                   last_level;
        uint32_t                   nr_samples;
        uint32_t                   flags;
        uint32_t                   padding;
};

struct virtio_gpu_transfer_3d {
        struct virtio_gpu_ctrl_hdr hdr;
        struct virtio_gpu_box      box;
        uint64_t                   offset;
        uint32_t                   resource_id;
        uint32_t                   level;
        uint32_t                   stride;
        uint32_t                   layer_stride;
};

struct virtio_gpu_submit_3d {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   size; // size of command buffer
        uint32_t                   padding;
};

/* Blob commands */

struct virtio_gpu_resource_create_blob {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   blob_mem;
        uint32_t                   blob_flags;
        uint32_t                   nr_entries;
        uint64_t                   blob_id;
        uint64_t                   size;
};

struct virtio_gpu_resource_map_blob {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   resource_id;
        uint32_t                   padding;
        uint64_t                   offset;
};

struct virtio_gpu_resp_map_info {
        struct virtio_gpu_ctrl_hdr hdr;
        uint32_t                   map_info;
        uint32_t                   padding;
};

/* Blob memory types */
#define VIRTIO_GPU_BLOB_MEM_GUEST        1
#define VIRTIO_GPU_BLOB_MEM_HOST3D       2
#define VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST 3

/* Blob flags */
#define VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE     (1 << 0)
#define VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE    (1 << 1)
#define VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE (1 << 2)

/* DRM UAPI - ioctl codes (match Linux virtgpu_drm.h exactly) */

#define DRM_VIRTGPU_MAP                  0x01
#define DRM_VIRTGPU_EXECBUFFER           0x02
#define DRM_VIRTGPU_GETPARAM             0x03
#define DRM_VIRTGPU_RESOURCE_CREATE      0x04
#define DRM_VIRTGPU_RESOURCE_INFO        0x05
#define DRM_VIRTGPU_TRANSFER_FROM_HOST   0x06
#define DRM_VIRTGPU_TRANSFER_TO_HOST     0x07
#define DRM_VIRTGPU_WAIT                 0x08
#define DRM_VIRTGPU_GET_CAPS             0x09
#define DRM_VIRTGPU_RESOURCE_CREATE_BLOB 0x0a
#define DRM_VIRTGPU_CONTEXT_INIT         0x0b

#define DRM_IOCTL_VIRTGPU_MAP                  DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_MAP, struct drm_virtgpu_map)
#define DRM_IOCTL_VIRTGPU_EXECBUFFER           DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_EXECBUFFER, struct drm_virtgpu_execbuffer)
#define DRM_IOCTL_VIRTGPU_GETPARAM             DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GETPARAM, struct drm_virtgpu_getparam)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE      DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE, struct drm_virtgpu_resource_create)
#define DRM_IOCTL_VIRTGPU_RESOURCE_INFO        DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_INFO, struct drm_virtgpu_resource_info)
#define DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST   DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_FROM_HOST, struct drm_virtgpu_3d_transfer)
#define DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST     DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_TRANSFER_TO_HOST, struct drm_virtgpu_3d_transfer)
#define DRM_IOCTL_VIRTGPU_WAIT                 DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_WAIT, struct drm_virtgpu_3d_wait)
#define DRM_IOCTL_VIRTGPU_GET_CAPS             DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_GET_CAPS, struct drm_virtgpu_get_caps)
#define DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_RESOURCE_CREATE_BLOB, struct drm_virtgpu_resource_create_blob)
#define DRM_IOCTL_VIRTGPU_CONTEXT_INIT         DRM_IOWR(DRM_COMMAND_BASE + DRM_VIRTGPU_CONTEXT_INIT, struct drm_virtgpu_context_init)

/* Param IDs */
#define DRM_VIRTGPU_PARAM_3D_FEATURES          1
#define DRM_VIRTGPU_PARAM_CAPSET_QUERY_FIX     2
#define DRM_VIRTGPU_PARAM_RESOURCE_BLOB        3
#define DRM_VIRTGPU_PARAM_HOST_VISIBLE         4
#define DRM_VIRTGPU_PARAM_CROSS_DEVICE         5
#define DRM_VIRTGPU_PARAM_CONTEXT_INIT         6
#define DRM_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs 7
#define DRM_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME  8
#define DRM_VIRTGPU_PARAM_BLOB_ALIGNMENT       9

/* DRM UAPI structs (fixed-width for x86-64 compat) */

struct drm_virtgpu_map {
        uint64_t offset; // mmap offset (in) / virtual addr (out)
        uint32_t handle;
        uint32_t pad;
};

struct drm_virtgpu_execbuffer {
        uint32_t flags;
        uint32_t size;
        uint64_t command;    // pointer to command buffer
        uint64_t bo_handles; // pointer to BO handle array
        uint32_t num_bo_handles;
        int32_t  fence_fd;
        uint32_t ring_idx;
        uint32_t syncobj_stride;
        uint32_t num_in_syncobjs;
        uint32_t num_out_syncobjs;
        uint64_t in_syncobjs;
        uint64_t out_syncobjs;
};

struct drm_virtgpu_getparam {
        uint64_t param;
        uint64_t value;
};

struct drm_virtgpu_resource_create {
        uint32_t target;
        uint32_t format;
        uint32_t bind;
        uint32_t width;
        uint32_t height;
        uint32_t depth;
        uint32_t array_size;
        uint32_t last_level;
        uint32_t nr_samples;
        uint32_t flags;
        uint32_t bo_handle;
        uint32_t res_handle;
        uint32_t size;
        uint32_t stride;
};

#define VIRTGPU_EXECBUF_FENCE_FD_IN  0x01
#define VIRTGPU_EXECBUF_FENCE_FD_OUT 0x02
#define VIRTGPU_EXECBUF_RING_IDX     0x04
#define VIRTGPU_EXECBUF_FLAGS        (VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT | VIRTGPU_EXECBUF_RING_IDX)

#define VIRTGPU_EXECBUF_SYNCOBJ_RESET 0x01
#define VIRTGPU_EXECBUF_SYNCOBJ_FLAGS VIRTGPU_EXECBUF_SYNCOBJ_RESET
struct drm_virtgpu_execbuffer_syncobj {
        uint32_t handle;
        uint32_t flags;
        uint64_t point;
};

struct drm_virtgpu_resource_info {
        uint32_t bo_handle;
        uint32_t res_handle;
        uint32_t size;
        uint32_t blob_mem;
};

struct drm_virtgpu_3d_transfer {
        uint32_t bo_handle;
        struct {
                uint32_t x;
                uint32_t y;
                uint32_t z;
                uint32_t w;
                uint32_t h;
                uint32_t d;
        } box;
        uint32_t level;
        uint32_t offset;
        uint32_t stride;
        uint32_t layer_stride;
};

struct drm_virtgpu_3d_wait {
        uint32_t handle;
        uint32_t flags;
};
#define VIRTGPU_WAIT_NOWAIT 0x01

struct drm_virtgpu_get_caps {
        uint32_t cap_set_id;
        uint32_t cap_set_ver;
        uint64_t addr; // user-space pointer for capset data
        uint32_t size;
        uint32_t pad;
};

struct drm_virtgpu_resource_create_blob {
        uint32_t blob_mem;
        uint32_t blob_flags;
        uint32_t bo_handle;
        uint32_t res_handle;
        uint64_t size;
        uint32_t pad;
        uint32_t cmd_size;
        uint64_t cmd;
        uint64_t blob_id;
        uint32_t blob_hints;
        uint32_t pad2;
};

#define DRM_VIRTGPU_BLOB_FLAG_HINT_DEFER_MAPPING 0x0001

#define VIRTGPU_CONTEXT_PARAM_CAPSET_ID       0x0001
#define VIRTGPU_CONTEXT_PARAM_NUM_RINGS       0x0002
#define VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK 0x0003
#define VIRTGPU_CONTEXT_PARAM_DEBUG_NAME      0x0004

struct drm_virtgpu_context_set_param {
        uint64_t param;
        uint64_t value;
};

struct drm_virtgpu_context_init {
        uint32_t num_params;
        uint32_t pad;
        uint64_t ctx_set_params;
};

#define VIRTGPU_EVENT_FENCE_SIGNALED 0x90000000

/* Internal resource / object types */

/* Capset IDs (matches virgl/drm virtgpu) */
#define VIRTGPU_CAPSET_VIRGL            1
#define VIRTGPU_CAPSET_VIRGL2           2
#define VIRTGPU_CAPSET_GFXSTREAM_VULKAN 3
#define VIRTGPU_CAPSET_VENUS            4
#define VIRTGPU_CAPSET_CROSS_DOMAIN     5
#define VIRTGPU_CAPSET_DRM              6

/* Per-context blob flags (kernel internal) */
#define VIRTGPU_CONTEXT_INIT_CAPSET_ID_MASK  0xff
#define VIRTGPU_CONTEXT_INIT_CAPSET_ID_SHIFT 0

/* Maximum resource ID managed by host */
#define VIRTGPU_RESOURCE_ID_INVALID 0

#define VIRTGPU_MAX_CONTEXT_RINGS 64
#define VIRTGPU_DEBUG_NAME_MAX    64

struct virtio_gpu_fpriv {
        uint32_t   ctx_id;
        uint32_t   context_init;
        uint32_t   num_rings;
        uint64_t   ring_idx_mask;
        bool       context_created;
        bool       explicit_debug_name;
        char       debug_name[VIRTGPU_DEBUG_NAME_MAX];
        spinlock_t context_lock;
};

/* Fence tracking */

struct virtio_gpu_fence {
        uint64_t             id;
        bool                 signaled;
        spinlock_t           lock;
        struct vp_virtqueue *emit_vq;
        struct list_head_fence {
                void *next;
        } node;
};

struct virtio_gpu_context_attachment {
        struct virtio_gpu_context_attachment *next;
        uint32_t                              ctx_id;
};

/* GEM object wrapper (virtio-gpu specific) */

struct virtio_gpu_object {
        struct drm_gem_object base;
        uint32_t              hw_res_handle; // host-side resource ID
        uint32_t              format;
        uint32_t              width;
        uint32_t              height;
        uint32_t              stride;
        uint32_t              depth;

        bool                                  created_3d;
        bool                                  created_blob;
        uint32_t                              blob_mem;
        uint32_t                              blob_flags;
        uint64_t                              blob_id;
        uint32_t                              ctx_id;
        bool                                  backing_attached;
        spinlock_t                            context_lock;
        struct virtio_gpu_context_attachment *context_attachments;

        /* Backing memory for guest-allocated resources */
        uint64_t                     backing_phys;
        size_t                       backing_page_count;
        uint32_t                     num_entries;
        struct virtio_gpu_mem_entry *entries;

        /* Pending fence */
        struct virtio_gpu_fence *fence;
};

/* Display mode from host */

struct virtio_gpu_display_mode {
        int                    width;
        int                    height;
        int                    vrefresh;
        bool                   enabled;
        struct virtio_gpu_rect rect;
};

struct virtio_gpu_capset_cache {
        uint32_t id;
        uint32_t max_version;
        uint32_t max_size;
        uint32_t cached_version;
        void    *data;
};

/* VirtIO-GPU device instance */

struct virtio_gpu_device {
        struct vp_device  *vp_dev;
        struct drm_device *drm_dev;

        /* Virtqueues: ctrlq (0), cursorq (1) */
        struct vp_virtqueue ctrlq;
        struct vp_virtqueue cursorq;
        spinlock_t          ctrlq_cmd_lock; // serialises synchronous batches
        spinlock_t          cursorq_cmd_lock;

        /* Feature flags negotiated */
        bool                           has_virgl;
        bool                           has_edid;
        bool                           has_resource_blob;
        bool                           has_context_init;
        uint32_t                       num_capsets;
        uint64_t                       capset_id_mask;
        spinlock_t                     capset_lock;
        struct virtio_gpu_capset_cache capsets[64];

        /* Display info from host */
        int                            num_scanouts;
        struct virtio_gpu_display_mode scanouts[16];

        /* Resource ID allocator (monotonic) */
        spinlock_t resource_idr_lock;
        uint32_t   next_resource_id;
        spinlock_t context_idr_lock;
        uint32_t   next_context_id;

        /* Fence tracking */
        spinlock_t             fence_lock;
        struct list_head_fence pending_fences;
        uint64_t               next_fence_id;

        /* Current framebuffer for scanout */
        struct drm_framebuffer   *current_fb;
        struct virtio_gpu_object *current_scanout_obj;
        struct drm_plane         *kms_primary;
        struct drm_crtc          *kms_crtc;
        struct drm_encoder       *kms_encoder;
        struct drm_connector     *kms_connector;
};

/* Function prototypes (defined across the virtgpu_*.c files) */

/* gpu.c - driver init / ioctls */
int                virtio_gpu_driver_init(void);
struct drm_device *virtio_gpu_dev_alloc(struct virtio_gpu_device *vgdev);

/* virtgpu_vq.c - virtqueue helpers */
int  virtgpu_vq_init(struct virtio_gpu_device *vgdev);
void virtgpu_vq_fini(struct virtio_gpu_device *vgdev);
int  virtgpu_ctrl_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size, void *resp, int resp_size, uint64_t *fence_id);
int  virtgpu_ctrl_cmd_batch(struct virtio_gpu_device *vgdev, struct virtgpu_vq_command *commands, uint32_t count);
int  virtgpu_cursor_cmd(struct virtio_gpu_device *vgdev, void *cmd, int cmd_size);

/* virtgpu_cmd.c - command encoding */
int virtgpu_cmd_get_display_info(struct virtio_gpu_device *vgdev);
int virtgpu_cmd_get_edid(struct virtio_gpu_device *vgdev, int scanout_id, void *edid, int *edid_size);
int virtgpu_cmd_create_resource_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj);
int virtgpu_cmd_create_resource_3d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct drm_virtgpu_resource_create *rc);
int virtgpu_cmd_create_blob(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct drm_virtgpu_resource_create_blob *blob);
int virtgpu_cmd_unref_resource(struct virtio_gpu_device *vgdev, uint32_t res_id);
int virtgpu_cmd_attach_backing(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj);
int virtgpu_cmd_detach_backing(struct virtio_gpu_device *vgdev, uint32_t res_id);
int virtgpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint64_t offset);
int virtgpu_cmd_transfer_to_host_2d_rect(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct drm_virtgpu_3d_transfer *xf);
int virtgpu_cmd_update_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct virtio_gpu_rect *rect, uint64_t offset);
int virtgpu_cmd_update_scanout_2d(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj, bool set_scanout);
int virtgpu_cmd_transfer_3d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id, const struct drm_virtgpu_3d_transfer *xf, bool to_host);
int virtgpu_cmd_resource_flush(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, struct virtio_gpu_rect *rect);
int virtgpu_cmd_set_scanout(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj);
int virtgpu_cmd_set_scanout_blob(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj);
int virtgpu_cmd_update_cursor(struct virtio_gpu_device *vgdev, uint32_t scanout_id, struct virtio_gpu_object *obj, int32_t x, int32_t y, int32_t hot_x, int32_t hot_y);
int virtgpu_cmd_move_cursor(struct virtio_gpu_device *vgdev, uint32_t scanout_id, int32_t x, int32_t y);
int virtgpu_cmd_ctx_create(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t context_init, const char *debug_name, uint32_t name_len);
int virtgpu_cmd_ctx_destroy(struct virtio_gpu_device *vgdev, uint32_t ctx_id);
int virtgpu_cmd_ctx_attach_resource(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t resource_id);
int virtgpu_cmd_ctx_detach_resource(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t resource_id);
int virtgpu_object_attach_context(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id);
int virtgpu_object_detach_context(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id);
int virtgpu_cmd_submit_3d(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t ring_idx, bool use_ring_idx, const void *cmd, uint32_t size, struct virtio_gpu_fence *fence);
int virtgpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, uint32_t idx, uint32_t *capset_id, uint32_t *max_version, uint32_t *max_size);
int virtgpu_cmd_get_capset(struct virtio_gpu_device *vgdev, uint32_t capset_id, uint32_t version, void *data, uint32_t max_size);

/* virtgpu_gem.c - GEM management */
struct virtio_gpu_object *virtgpu_gem_alloc_object(struct drm_device *dev, size_t size);
void                      virtgpu_gem_free_object(struct drm_gem_object *obj);
int                       virtgpu_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev, struct drm_mode_create_dumb *args);
int                       virtgpu_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle, uint64_t *offset);
int                       virtgpu_gem_prime_export(struct drm_device *dev, struct drm_gem_object *obj, int *prime_fd);
struct drm_gem_object    *virtgpu_gem_prime_import(struct drm_device *dev, void *dma_buf);

/* Module initialisation - called from kernel init after drm_init() */
int   virtio_gpu_init(void);
void  virtio_gpu_module_exit(void);
void *virtio_gpu_get_device(void);

/* virtgpu_kms.c - KMS display pipeline */
int  virtgpu_kms_init(struct virtio_gpu_device *vgdev);
void virtgpu_kms_fini(struct virtio_gpu_device *vgdev);
int  virtgpu_kms_get_modes(struct drm_connector *connector);

/* DRM fourcc - VirtIO GPU format translation */

/*
 * Convert a DRM fourcc pixel format to the corresponding VirtIO GPU
 * format constant.  On little-endian x86 the byte-order-in-memory
 * names used by the VirtIO spec map as follows:
 *
 *   DRM_FORMAT_XRGB8888 -> VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM  (2)
 *   DRM_FORMAT_ARGB8888 -> VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM  (1)
 * Scanout only exposes the two Linux virtgpu 2D plane formats.  Other
 * formats need a negotiated 3D path and must not be silently reinterpreted.
 *
 * Returns the VirtIO GPU format code, or 0 if unsupported.
 */
static inline uint32_t virtgpu_drm_format_to_virtio(uint32_t drm_fourcc)
{
    switch (drm_fourcc) {
        case DRM_FORMAT_XRGB8888 :
            return VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM;
        case DRM_FORMAT_ARGB8888 :
            return VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM;
        default :
            return 0;
    }
}

/* Resource ID management */
static inline uint32_t virtgpu_resource_id_alloc(struct virtio_gpu_device *vgdev)
{
    uint32_t id;

    spin_lock(&vgdev->resource_idr_lock);
    id = vgdev->next_resource_id++;
    if (id == VIRTGPU_RESOURCE_ID_INVALID) id = vgdev->next_resource_id++;
    spin_unlock(&vgdev->resource_idr_lock);
    return id;
}

/* Page-flip helper used by the KMS atomic commit path. */
int virtgpu_page_flip(struct virtio_gpu_device *vgdev, struct drm_framebuffer *fb, struct drm_framebuffer *old_fb);

/* Framebuffer functions provided by the KMS driver. */
extern const struct drm_framebuffer_funcs virtgpu_fb_funcs;

#endif // INCLUDE_VIRTGPU_DRV_H_
