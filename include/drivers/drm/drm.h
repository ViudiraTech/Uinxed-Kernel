/*
 *
 *      drm.h
 *      Direct Rendering Manager core UAPI
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux DRM UAPI (include/uapi/drm/drm.h). Layouts are
 *  byte-compatible with Linux on x86-64: pointer and `unsigned long` UAPI
 *  fields are expressed as fixed-width `uint64_t` so the in-kernel view is
 *  stable regardless of the host pointer model.
 *
 */

#ifndef INCLUDE_DRM_DRM_H_
#define INCLUDE_DRM_DRM_H_

#include <libs/std/stddef.h>
#include <libs/std/stdint.h>

/* Fixed-width UAPI aliases mirroring Linux __u8/__s8/... */
typedef int8_t   __s8;
typedef uint8_t  __u8;
typedef int16_t  __s16;
typedef uint16_t __u16;
typedef int32_t  __s32;
typedef uint32_t __u32;
typedef int64_t  __s64;
typedef uint64_t __u64;
typedef uint32_t drm_handle_t;
typedef uint32_t drm_context_t;
typedef uint32_t drm_drawable_t;
typedef uint32_t drm_magic_t;

/* ioctl encoding macros, x86-64 identical to Linux asm-generic/ioctl.h.
 * Guarded with #ifndef so they coexist with input_event.h which also
 * defines them (the project has no single asm/ioctl.h header). */
#ifndef _IOC_NRBITS
#    define _IOC_NRBITS 8
#endif
#ifndef _IOC_TYPEBITS
#    define _IOC_TYPEBITS 8
#endif
#ifndef _IOC_SIZEBITS
#    define _IOC_SIZEBITS 14
#endif
#ifndef _IOC_DIRBITS
#    define _IOC_DIRBITS 2
#endif

#ifndef _IOC_NONE
#    define _IOC_NONE 0U
#endif
#ifndef _IOC_WRITE
#    define _IOC_WRITE 1U
#endif
#ifndef _IOC_READ
#    define _IOC_READ 2U
#endif

#ifndef _IOC
#    define _IOC(dir, type, nr, size) (((dir) << 30) | ((type) << 8) | (nr) | ((size) << 16))
#endif

#ifndef _IO
#    define _IO(type, nr) _IOC(_IOC_NONE, (type), (nr), 0)
#endif
#ifndef _IOR
#    define _IOR(type, nr, size) _IOC(_IOC_READ, (type), (nr), (uint32_t)sizeof(size))
#endif
#ifndef _IOW
#    define _IOW(type, nr, size) _IOC(_IOC_WRITE, (type), (nr), (uint32_t)sizeof(size))
#endif
#ifndef _IOWR
#    define _IOWR(type, nr, size) _IOC(_IOC_READ | _IOC_WRITE, (type), (nr), (uint32_t)sizeof(size))
#endif

#ifndef _IOC_DIR
#    define _IOC_DIR(cmd) (((cmd) >> 30) & 0x3)
#endif
#ifndef _IOC_TYPE
#    define _IOC_TYPE(cmd) (((cmd) >> 8) & 0xff)
#endif
#ifndef _IOC_NR
#    define _IOC_NR(cmd) ((cmd) & 0xff)
#endif
#ifndef _IOC_SIZE
#    define _IOC_SIZE(cmd) (((cmd) >> 16) & 0x3fff)
#endif

#define DRM_IOCTL_BASE 'd'

#define DRM_IO(nr)         _IO(DRM_IOCTL_BASE, nr)
#define DRM_IOR(nr, type)  _IOR(DRM_IOCTL_BASE, nr, type)
#define DRM_IOW(nr, type)  _IOW(DRM_IOCTL_BASE, nr, type)
#define DRM_IOWR(nr, type) _IOWR(DRM_IOCTL_BASE, nr, type)

#define DRM_NAME        "drm"
#define DRM_MIN_ORDER   5
#define DRM_MAX_ORDER   22
#define DRM_RAM_PERCENT 10

#define _DRM_LOCK_HELD             0x80000000U
#define _DRM_LOCK_CONT             0x40000000U
#define _DRM_LOCK_IS_HELD(lock)    ((lock) & _DRM_LOCK_HELD)
#define _DRM_LOCK_IS_CONT(lock)    ((lock) & _DRM_LOCK_CONT)
#define _DRM_LOCKING_CONTEXT(lock) ((lock) & ~(_DRM_LOCK_HELD | _DRM_LOCK_CONT))

/* Clip rectangle (legacy) */
struct drm_clip_rect {
        __u16 x1;
        __u16 y1;
        __u16 x2;
        __u16 y2;
};

struct drm_drawable_info {
        __u32                 num_rects;
        struct drm_clip_rect *rects;
};

struct drm_tex_region {
        __u8  next;
        __u8  prev;
        __u8  in_use;
        __u8  padding;
        __u32 age;
};

struct drm_hw_lock {
        volatile __u32 lock;
        char           padding[60];
};

/* DRM_IOCTL_VERSION */
struct drm_version {
        __s32 version_major;
        __s32 version_minor;
        __s32 version_patchlevel;
        __u64 name_len; // Length of name buffer
        __u64 name;     // Pointer to name buffer (user)
        __u64 date_len; // Length of date buffer
        __u64 date;     // Pointer to date buffer (user)
        __u64 desc_len; // Length of desc buffer
        __u64 desc;     // Pointer to desc buffer (user)
};

/* DRM_IOCTL_GET_UNIQUE / DRM_IOCTL_SET_UNIQUE */
struct drm_unique {
        __u64 unique_len;
        __u64 unique;
};

struct drm_list {
        __s32 count;
        __u64 version;
};

struct drm_block {
        __s32 unused;
};

/* DRM_IOCTL_CONTROL */
struct drm_control {
        __s32 func;
        __s32 irq;
};
#define DRM_ADD_COMMAND    0
#define DRM_RM_COMMAND     1
#define DRM_INST_HANDLER   2
#define DRM_UNINST_HANDLER 3

enum drm_map_type {
    _DRM_FRAME_BUFFER   = 0,
    _DRM_REGISTERS      = 1,
    _DRM_SHM            = 2,
    _DRM_AGP            = 3,
    _DRM_SCATTER_GATHER = 4,
    _DRM_CONSISTENT     = 5,
};

enum drm_map_flags {
    _DRM_RESTRICTED      = 0x01,
    _DRM_READ_ONLY       = 0x02,
    _DRM_LOCKED          = 0x04,
    _DRM_KERNEL          = 0x08,
    _DRM_WRITE_COMBINING = 0x10,
    _DRM_CONTAINS_LOCK   = 0x20,
    _DRM_REMOVABLE       = 0x40,
    _DRM_DRIVER          = 0x80,
};

struct drm_ctx_priv_map {
        __u32 ctx_id;
        __u64 handle;
};

struct drm_map {
        __u64              offset; // Requested physical address
        __u64              size;   // Requested physical size in bytes
        enum drm_map_type  type;
        enum drm_map_flags flags;
        __u64              handle; // User "handle" / kernel-virtual address
        __s32              mtrr;   // MTRR slot used
};

/* DRM_IOCTL_GET_CLIENT */
struct drm_client {
        __s32 idx;
        __s32 auth;
        __u64 pid;
        __u64 uid;
        __u64 magic;
        __u64 iocs;
};

enum drm_stat_type {
    _DRM_STAT_LOCK,
    _DRM_STAT_OPENS,
    _DRM_STAT_CLOSES,
    _DRM_STAT_IOCTLS,
    _DRM_STAT_LOCKS,
    _DRM_STAT_UNLOCKS,
    _DRM_STAT_VALUE,
    _DRM_STAT_BYTE,
    _DRM_STAT_COUNT,
    _DRM_STAT_IRQ,
    _DRM_STAT_PRIMARY,
    _DRM_STAT_SECONDARY,
    _DRM_STAT_DMA,
    _DRM_STAT_SPECIAL,
    _DRM_STAT_MISSED,
};

struct drm_stats {
        __u64 count;
        struct {
                __u64              value;
                enum drm_stat_type type;
        } data[15];
};

enum drm_lock_flags {
    _DRM_LOCK_READY      = 0x01,
    _DRM_LOCK_QUIESCENT  = 0x02,
    _DRM_LOCK_FLUSH      = 0x04,
    _DRM_LOCK_FLUSH_ALL  = 0x08,
    _DRM_HALT_ALL_QUEUES = 0x10,
    _DRM_HALT_CUR_QUEUES = 0x20,
};

struct drm_lock {
        __s32               context;
        enum drm_lock_flags flags;
};

enum drm_dma_flags {
    _DRM_DMA_BLOCK        = 0x01,
    _DRM_DMA_WHILE_LOCKED = 0x02,
    _DRM_DMA_PRIORITY     = 0x04,
    _DRM_DMA_WAIT         = 0x10,
    _DRM_DMA_SMALLER_OK   = 0x20,
    _DRM_DMA_LARGER_OK    = 0x40,
};

struct drm_buf_desc {
        __s32 count;
        __s32 size;
        __s32 low_mark;
        __s32 high_mark;
        __u32 flags;
        __u64 agp_start;
};
#define _DRM_PAGE_ALIGN    0x01
#define _DRM_AGP_BUFFER    0x02
#define _DRM_SG_BUFFER     0x04
#define _DRM_FB_BUFFER     0x08
#define _DRM_PCI_BUFFER_RO 0x10

struct drm_buf_info {
        __s32 count;
        __u64 list;
};

struct drm_buf_free {
        __s32 count;
        __u64 list;
};

struct drm_buf_pub {
        __s32 idx;
        __s32 total;
        __s32 used;
        __u64 address;
};

struct drm_buf_map {
        __s32 count;
        __u64 virtual_; // Mmap'd area in user-virtual
        __u64 list;     // drm_buf_pub array (user)
};

struct drm_dma {
        __s32              context;
        __s32              send_count;
        __u64              send_indices;
        __u64              send_sizes;
        enum drm_dma_flags flags;
        __s32              request_count;
        __s32              request_size;
        __u64              request_indices;
        __u64              request_sizes;
        __s32              granted_count;
};

enum drm_ctx_flags {
    _DRM_CONTEXT_PRESERVED = 0x01,
    _DRM_CONTEXT_2DONLY    = 0x02,
};

struct drm_ctx {
        drm_context_t      handle;
        enum drm_ctx_flags flags;
};

struct drm_ctx_res {
        __s32 count;
        __u64 contexts;
};

struct drm_draw {
        drm_drawable_t handle;
};

typedef enum { DRM_DRAWABLE_CLIPRECTS } drm_drawable_info_type_t;

struct drm_update_draw {
        drm_drawable_t handle;
        __u32          type;
        __u32          num;
        __u64          data;
};

/* DRM_IOCTL_GET_MAGIC / DRM_IOCTL_AUTH_MAGIC */
struct drm_auth {
        drm_magic_t magic;
};

/* DRM_IOCTL_IRQ_BUSID */
struct drm_irq_busid {
        __s32 irq;
        __s32 busnum;
        __s32 devnum;
        __s32 funcnum;
};

enum drm_vblank_seq_type {
    _DRM_VBLANK_ABSOLUTE       = 0x0,
    _DRM_VBLANK_RELATIVE       = 0x1,
    _DRM_VBLANK_HIGH_CRTC_MASK = 0x0000003e,
    _DRM_VBLANK_EVENT          = 0x4000000,
    _DRM_VBLANK_FLIP           = 0x8000000,
    _DRM_VBLANK_NEXTONMISS     = 0x10000000,
    _DRM_VBLANK_SECONDARY      = 0x20000000,
    _DRM_VBLANK_SIGNAL         = 0x40000000,
};
#define _DRM_VBLANK_HIGH_CRTC_SHIFT 1
#define _DRM_VBLANK_TYPES_MASK      (_DRM_VBLANK_ABSOLUTE | _DRM_VBLANK_RELATIVE)
#define _DRM_VBLANK_FLAGS_MASK      (_DRM_VBLANK_EVENT | _DRM_VBLANK_SIGNAL | _DRM_VBLANK_SECONDARY | _DRM_VBLANK_NEXTONMISS)

struct drm_wait_vblank_request {
        enum drm_vblank_seq_type type;
        __u32                    sequence;
        __u64                    signal;
};

struct drm_wait_vblank_reply {
        enum drm_vblank_seq_type type;
        __u32                    sequence;
        __s64                    tval_sec;
        __s64                    tval_usec;
};

union drm_wait_vblank {
        struct drm_wait_vblank_request request;
        struct drm_wait_vblank_reply   reply;
};

#define _DRM_PRE_MODESET  1
#define _DRM_POST_MODESET 2

struct drm_modeset_ctl {
        __u32 crtc;
        __u32 cmd;
};

/* AGP UAPI (legacy, retained for binary compatibility) */
struct drm_agp_mode {
        __u64 mode;
};
struct drm_agp_buffer {
        __u64 size;
        __u64 handle;
        __u64 type;
        __u64 physical;
};
struct drm_agp_binding {
        __u64 handle;
        __u64 offset;
};
struct drm_agp_info {
        __s32 agp_version_major;
        __s32 agp_version_minor;
        __u64 mode;
        __u64 aperture_base;
        __u64 aperture_size;
        __u64 memory_allowed;
        __u64 memory_used;
        __u16 id_vendor;
        __u16 id_device;
};
struct drm_scatter_gather {
        __u64 size;
        __u64 handle;
};

/* DRM_IOCTL_SET_VERSION */
struct drm_set_version {
        __s32 drm_di_major;
        __s32 drm_di_minor;
        __s32 drm_dd_major;
        __s32 drm_dd_minor;
};

/* DRM_IOCTL_GEM_CLOSE / FLINK / OPEN */
struct drm_gem_close {
        __u32 handle;
        __u32 pad;
};
struct drm_gem_flink {
        __u32 handle;
        __u32 name;
};
struct drm_gem_open {
        __u32 name;
        __u32 handle;
        __u64 size;
};

/* PRIME capability flags */
#define DRM_PRIME_CAP_EXPORT 1
#define DRM_PRIME_CAP_IMPORT 2

/* DRM_IOCTL_GET_CAP / SET_CLIENT_CAP */
#define DRM_CAP_DUMB_BUFFER            0x1
#define DRM_CAP_VBLANK_HIGH_CRTC       0x2
#define DRM_CAP_DUMB_PREFERRED_DEPTH   0x3
#define DRM_CAP_DUMB_PREFER_SHADOW     0x4
#define DRM_CAP_PRIME                  0x5
#define DRM_CAP_TIMESTAMP_MONOTONIC    0x6
#define DRM_CAP_ASYNC_PAGE_FLIP        0x7
#define DRM_CAP_CURSOR_WIDTH           0x8
#define DRM_CAP_CURSOR_HEIGHT          0x9
#define DRM_CAP_ADDFB2_MODIFIERS       0x10
#define DRM_CAP_PAGE_FLIP_TARGET       0x11
#define DRM_CAP_CRTC_IN_VBLANK_EVENT   0x12
#define DRM_CAP_SYNCOBJ                0x13
#define DRM_CAP_SYNCOBJ_TIMELINE       0x14
#define DRM_CAP_ATOMIC_ASYNC_PAGE_FLIP 0x15

struct drm_get_cap {
        __u64 capability;
        __u64 value;
};

#define DRM_CLIENT_CAP_STEREO_3D            1
#define DRM_CLIENT_CAP_UNIVERSAL_PLANES     2
#define DRM_CLIENT_CAP_ATOMIC               3
#define DRM_CLIENT_CAP_ASPECT_RATIO         4
#define DRM_CLIENT_CAP_WRITEBACK_CONNECTORS 5
#define DRM_CLIENT_CAP_CURSOR_PLANE_HOTSPOT 6

struct drm_set_client_cap {
        __u64 capability;
        __u64 value;
};

/* DRM_IOCTL_PRIME_HANDLE_TO_FD / PRIME_FD_TO_HANDLE (dma-buf) */
struct drm_prime_handle {
        __s32 fd;
        __u32 handle;
        __u32 flags;
};

/*
 * ioctl command numbers. Order and values match Linux exactly so that
 * libdrm-built userspace drives this kernel without recompilation.
 */
#define DRM_IOCTL_VERSION     DRM_IOWR(0x00, struct drm_version)
#define DRM_IOCTL_GET_UNIQUE  DRM_IOWR(0x01, struct drm_unique)
#define DRM_IOCTL_GET_MAGIC   DRM_IOR(0x02, struct drm_auth)
#define DRM_IOCTL_IRQ_BUSID   DRM_IOWR(0x03, struct drm_irq_busid)
#define DRM_IOCTL_GET_MAP     DRM_IOWR(0x04, struct drm_map)
#define DRM_IOCTL_GET_CLIENT  DRM_IOWR(0x05, struct drm_client)
#define DRM_IOCTL_GET_STATS   DRM_IOR(0x06, struct drm_stats)
#define DRM_IOCTL_SET_VERSION DRM_IOWR(0x07, struct drm_set_version)
#define DRM_IOCTL_MODESET_CTL DRM_IOWR(0x08, struct drm_modeset_ctl)

#define DRM_IOCTL_GEM_CLOSE      DRM_IOW(0x09, struct drm_gem_close)
#define DRM_IOCTL_GEM_FLINK      DRM_IOWR(0x0a, struct drm_gem_flink)
#define DRM_IOCTL_GEM_OPEN       DRM_IOWR(0x0b, struct drm_gem_open)
#define DRM_IOCTL_GET_CAP        DRM_IOWR(0x0c, struct drm_get_cap)
#define DRM_IOCTL_SET_CLIENT_CAP DRM_IOW(0x0d, struct drm_set_client_cap)

#define DRM_IOCTL_SET_UNIQUE DRM_IOW(0x10, struct drm_unique)
#define DRM_IOCTL_AUTH_MAGIC DRM_IOW(0x11, struct drm_auth)
#define DRM_IOCTL_BLOCK      DRM_IOWR(0x12, struct drm_block)
#define DRM_IOCTL_UNBLOCK    DRM_IOWR(0x13, struct drm_block)
#define DRM_IOCTL_CONTROL    DRM_IOWR(0x14, struct drm_control)

#define DRM_IOCTL_ADD_MAP DRM_IOWR(0x15, struct drm_map)
#define DRM_IOCTL_RM_MAP  DRM_IOW(0x1b, struct drm_map)

#define DRM_IOCTL_SET_SAREA_CTX DRM_IOW(0x1c, struct drm_ctx_priv_map)
#define DRM_IOCTL_GET_SAREA_CTX DRM_IOWR(0x1d, struct drm_ctx_priv_map)

#define DRM_IOCTL_SET_MASTER  DRM_IO(0x1e)
#define DRM_IOCTL_DROP_MASTER DRM_IO(0x1f)

#define DRM_IOCTL_ADD_CTX    DRM_IOWR(0x20, struct drm_ctx)
#define DRM_IOCTL_RM_CTX     DRM_IOWR(0x21, struct drm_ctx)
#define DRM_IOCTL_MOD_CTX    DRM_IOW(0x22, struct drm_ctx)
#define DRM_IOCTL_GET_CTX    DRM_IOWR(0x23, struct drm_ctx)
#define DRM_IOCTL_SWITCH_CTX DRM_IOW(0x24, struct drm_ctx)
#define DRM_IOCTL_NEW_CTX    DRM_IOW(0x25, struct drm_ctx)
#define DRM_IOCTL_RES_CTX    DRM_IOWR(0x26, struct drm_ctx_res)

#define DRM_IOCTL_ADD_DRAW DRM_IOWR(0x27, struct drm_draw)
#define DRM_IOCTL_RM_DRAW  DRM_IOWR(0x28, struct drm_draw)

#define DRM_IOCTL_DMA    DRM_IOWR(0x29, struct drm_dma)
#define DRM_IOCTL_LOCK   DRM_IOW(0x2a, struct drm_lock)
#define DRM_IOCTL_UNLOCK DRM_IOW(0x2b, struct drm_lock)
#define DRM_IOCTL_FINISH DRM_IOW(0x2c, struct drm_lock)

#define DRM_IOCTL_PRIME_HANDLE_TO_FD DRM_IOWR(0x2d, struct drm_prime_handle)
#define DRM_IOCTL_PRIME_FD_TO_HANDLE DRM_IOWR(0x2e, struct drm_prime_handle)

#define DRM_IOCTL_AGP_ACQUIRE DRM_IO(0x30)
#define DRM_IOCTL_AGP_RELEASE DRM_IO(0x31)
#define DRM_IOCTL_AGP_ENABLE  DRM_IOW(0x32, struct drm_agp_mode)
#define DRM_IOCTL_AGP_INFO    DRM_IOR(0x33, struct drm_agp_info)
#define DRM_IOCTL_AGP_ALLOC   DRM_IOWR(0x34, struct drm_agp_buffer)
#define DRM_IOCTL_AGP_FREE    DRM_IOW(0x35, struct drm_agp_buffer)
#define DRM_IOCTL_AGP_BIND    DRM_IOW(0x36, struct drm_agp_binding)
#define DRM_IOCTL_AGP_UNBIND  DRM_IOW(0x37, struct drm_agp_binding)

#define DRM_IOCTL_SG_ALLOC DRM_IOWR(0x38, struct drm_scatter_gather)
#define DRM_IOCTL_SG_FREE  DRM_IOW(0x39, struct drm_scatter_gather)

#define DRM_IOCTL_WAIT_VBLANK DRM_IOWR(0x3a, union drm_wait_vblank)
#define DRM_IOCTL_UPDATE_DRAW DRM_IOW(0x3f, struct drm_update_draw)

/* Driver-private ioctls (0x40..0x9f). Drivers install their own table. */
#define DRM_COMMAND_BASE 0x40
#define DRM_COMMAND_END  0xa0

/* Mode setting ioctls begin at 0xa0; defined in drm_mode.h */

#endif /* INCLUDE_DRM_DRM_H_ */
