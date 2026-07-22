/*
 *
 *      drm_device.h
 *      DRM central contract header (device / driver / file / KMS objects)
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Adapted from the Linux DRM core type definitions (include/drm/drm_*.h).
 *  This is the single source of truth for cross-module type layout; every
 *  KMS object header includes it. Pointers to user-space / file-system /
 *  bus objects are intentionally opaque (void *) so the DRM core remains
 *  independent of the rest of the kernel.
 *
 */

#ifndef INCLUDE_DRM_DRM_DEVICE_H_
#define INCLUDE_DRM_DRM_DEVICE_H_

#include <drm/drm.h>
#include <drm/drm_hashtab.h>
#include <drm/drm_mode.h>
#include <drm/drm_color_mgmt.h>
#include <drm/drm_fourcc.h>
#include <drm/drm_idr.h>
#include <drm/drm_mm.h>
#include <drm/drm_modeset_lock.h>
#include <drm/drm_rect.h>
#include <intrusive_list.h>
#include <spin_lock.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ------------------------------------------------------------------ */
/* Forward declarations                                               */
/* ------------------------------------------------------------------ */

struct drm_device;
struct drm_file;
struct drm_driver;
struct drm_mode_object;
struct drm_property;
struct drm_property_blob;
struct drm_display_mode;
struct drm_mode_config;
struct drm_crtc;
struct drm_crtc_state;
struct drm_connector;
struct drm_connector_state;
struct drm_encoder;
struct drm_plane;
struct drm_plane_state;
struct drm_framebuffer;
struct drm_gem_object;
struct drm_atomic_state;
struct drm_pending_vblank_event;
struct drm_master;
struct drm_minor;
struct drm_mode_set;
struct drm_fb_helper;
struct drm_vblank_crtc;
struct drm_private_obj;
struct drm_private_state;

/* ------------------------------------------------------------------ */
/* Kernel-internal enums (not UAPI; mirror Linux include/drm/drm_*.h) */
/* ------------------------------------------------------------------ */

enum drm_mode_status {
    MODE_OK         = 0,
    MODE_HSYNC      = 1,
    MODE_VSYNC      = 2,
    MODE_H_ILLEGAL  = 3,
    MODE_V_ILLEGAL  = 4,
    MODE_BAD_WIDTH  = 5,
    MODE_NOMODE     = 6,
    MODE_NO_INTERLACE = 7,
    MODE_NO_DBLESCAN  = 8,
    MODE_NO_VSCAN     = 9,
    MODE_MEM          = 10,
    MODE_VIRTUAL_X    = 11,
    MODE_VIRTUAL_Y    = 12,
    MODE_MEM_VIRT     = 13,
    MODE_NOCLOCK      = 14,
    MODE_CLOCK_HIGH   = 15,
    MODE_CLOCK_LOW    = 16,
    MODE_CLOCK_RANGE  = 17,
    MODE_BAD_HVALUE   = 18,
    MODE_BAD_VVALUE   = 19,
    MODE_BAD_VSCAN    = 20,
    MODE_HSYNC_NARROW = 21,
    MODE_HSYNC_WIDE   = 22,
    MODE_HBLANK_NARROW = 23,
    MODE_HBLANK_WIDE  = 24,
    MODE_VSYNC_NARROW = 25,
    MODE_VSYNC_WIDE   = 26,
    MODE_VBLANK_NARROW = 27,
    MODE_VBLANK_WIDE  = 28,
    MODE_PANEL        = 29,
    MODE_INTERLACE_WIDTH = 30,
    MODE_ONE_WIDTH    = 31,
    MODE_ONE_HEIGHT   = 32,
    MODE_ONE_SIZE     = 33,
    MODE_NO_REDUCED   = 34,
    MODE_NO_STEREO    = 35,
    MODE_NO_420       = 36,
    MODE_STALE        = -1,
    MODE_BAD          = -2,
    MODE_ERROR        = -3,
};

enum hdmi_picture_aspect {
    HDMI_PICTURE_ASPECT_NONE,
    HDMI_PICTURE_ASPECT_4_3,
    HDMI_PICTURE_ASPECT_16_9,
    HDMI_PICTURE_ASPECT_64_27,
    HDMI_PICTURE_ASPECT_256_135,
    HDMI_PICTURE_ASPECT_RESERVED,
};

enum drm_plane_type {
    DRM_PLANE_TYPE_OVERLAY,
    DRM_PLANE_TYPE_PRIMARY,
    DRM_PLANE_TYPE_CURSOR,
};

enum drm_connector_status {
    connector_status_connected = 1,
    connector_status_disconnected = 2,
    connector_status_unknown = 3,
};

enum drm_connector_force {
    DRM_FORCE_UNSPECIFIED,
    DRM_FORCE_OFF,
    DRM_FORCE_ON,
    DRM_FORCE_ON_DIGITAL,
};

/* Per-object property value store (object_id -> value). */
struct drm_property_set {
    spinlock_t              lock;
    uint32_t                count;
    uint32_t                capacity;
    uint32_t               *ids;     /* parallel arrays */
    uint64_t               *values;
};

struct drm_mode_object {
    uint32_t                id;    /* userspace-visible object id */
    uint32_t                type;  /* one of DRM_MODE_OBJECT_* */
    struct drm_device      *dev;
    int                     refcount;
    spinlock_t              ref_lock;
    struct drm_property_set *properties; /* optional per-object property set */
};

/* Generic object lookup by id+type. Returns NULL if not found. */
struct drm_mode_object *drm_mode_object_find(struct drm_device *dev, struct drm_file *file_priv, uint32_t id,
                                             uint32_t type);

void drm_mode_object_get(struct drm_mode_object *obj);
void drm_mode_object_put(struct drm_mode_object *obj);

struct drm_property_enum {
    uint64_t              value;
    char                  name[DRM_PROP_NAME_LEN];
    ilist_node_t          head; /* in property->enum_list */
};

struct drm_property {
    struct drm_mode_object base;
    char                   name[DRM_PROP_NAME_LEN];
    uint32_t               flags;
    uint32_t               num_values;
    uint64_t              *values; /* range [lo,hi] / enum values / bitmask values */
    ilist_node_t           enum_list; /* head of drm_property_enum.head */
    ilist_node_t           dev_head;  /* node in mode_config.property_list */
    struct drm_device     *dev;
};

struct drm_property_blob {
    struct drm_mode_object base;
    void                  *data;
    size_t                 length;
    ilist_node_t           head_global; /* in mode_config.property_blob_list */
    ilist_node_t           head_file;   /* optional: in file legacy_blob_list */
};

/* ------------------------------------------------------------------ */
/* Display modes                                                      */
/* ------------------------------------------------------------------ */

struct drm_display_mode {
    struct drm_mode_object base;
    char                   name[DRM_DISPLAY_MODE_LEN];
    int                    connector_count;
    ilist_node_t           head; /* in connector->modes */
    enum drm_mode_status   status;

    int                    clock; /* kHz */
    int                    hdisplay, hsync_start, hsync_end, htotal, hskew;
    int                    vdisplay, vsync_start, vsync_end, vtotal, vscan;
    unsigned int           flags;
    int                    crtc_clock; /* actual scanout clock (after TMDS) */
    int                    crtc_hdisplay, crtc_hblank_start, crtc_hblank_end;
    int                    crtc_hsync_start, crtc_hsync_end, crtc_htotal;
    int                    crtc_vdisplay, crtc_vblank_start, crtc_vblank_end;
    int                    crtc_vsync_start, crtc_vsync_end, crtc_vtotal;
    int                    width_mm;
    int                    height_mm;
    int                    vrefresh;
    int                    hsync;
    enum hdmi_picture_aspect picture_aspect_ratio;
    unsigned int           type; /* DRM_MODE_TYPE_* */

    /* private head for mode_config.usermode_list */
    ilist_node_t           usermode_head;
};

/* ------------------------------------------------------------------ */
/* KMS object forward structures (minimal; full defs in their headers) */
/* ------------------------------------------------------------------ */

struct drm_crtc {
    struct drm_device      *dev;
    struct drm_mode_object  base;
    struct drm_plane       *primary;
    struct drm_plane       *cursor;
    struct drm_plane       *legacy_cursor;
    struct drm_modeset_lock mutex;
    struct drm_mode_config *mode_config;
    ilist_node_t            head; /* in mode_config.crtc_list */
    int                     index;
    spinlock_t              commit_lock;
    struct drm_crtc_state  *state;
    struct drm_crtc_state  *commit_state;
    bool                    enabled;
    struct drm_display_mode mode;
    struct drm_display_mode saved_mode;
    int                     x, y;
    uint32_t                gamma_size;
    uint16_t               *gamma_store;
    spinlock_t              spinlock; /* legacy cursor lock */
    void                   *helper_private;
};

struct drm_plane {
    struct drm_device      *dev;
    struct drm_mode_object  base;
    uint32_t                possible_crtcs;
    uint32_t               *format_types;
    unsigned int            format_count;
    uint64_t               *modifiers;
    unsigned int            modifier_count;
    enum drm_plane_type     type;
    unsigned int            zpos_property_default;
    ilist_node_t            head; /* in mode_config.plane_list (or crtc list) */
    struct drm_modeset_lock mutex;
    struct drm_plane_state *state;
    char                   *name;
    void                   *helper_private;
    uint32_t                crtc_id;  /* currently attached CRTC (legacy ioctl tracking) */
    uint32_t                fb_id;    /* currently attached FB (legacy ioctl tracking) */
};

struct drm_encoder {
    struct drm_device      *dev;
    struct drm_mode_object  base;
    uint32_t                encoder_type;
    uint32_t                possible_crtcs;
    uint32_t                possible_clones;
    struct drm_crtc        *crtc;
    ilist_node_t            head; /* in mode_config.encoder_list */
    const struct drm_connector *connector_mask_list_head_unused;
    void                   *helper_private;
};

struct drm_connector {
    struct drm_device        *dev;
    struct drm_mode_object    base;
    char                      name[DRM_CONNECTOR_NAME_LEN];
    uint32_t                  connector_type;
    uint32_t                  connector_type_id;
    bool                      interlace_allowed, doublescan_allowed, stereo_allowed;
    uint32_t                  ycbcr_420_allowed;
    enum drm_connector_status status;
    struct list_head_unused  { void *n; } probed_modes_anchor; /* placeholder */
    ilist_node_t              modes;      /* head of drm_display_mode.head */
    ilist_node_t              user_modes; /* head of usermode_head */
    struct drm_display_mode  *modes_ptr_array_placeholder;
    uint32_t                  display_info_width_mm, display_info_height_mm;
    uint8_t                  *eld;
    uint8_t                  *edid_blob_ptr;
    spinlock_t                edid_lock;
    int                       null_edid_counter;
    bool                      override_edid;
    struct drm_property_blob *edid_blob;
    struct drm_property_blob *path_blob;
    struct drm_property_blob *tile_blob;
    ilist_node_t              head;       /* in mode_config.connector_list */
    struct drm_modeset_lock   mutex;
    enum drm_connector_force  force;
    bool                      override_edid_set;
    struct drm_connector_state *state;
    void                     *helper_private;
    uint32_t                  possible_encoders_count;
    uint32_t                 *possible_encoders_ids;
};

struct drm_framebuffer {
    struct drm_mode_object    base;
    uint32_t                  format; /* DRM_FORMAT_* fourcc */
    uint64_t                  modifier;
    unsigned int              width;
    unsigned int              height;
    unsigned int              pitches[4];
    unsigned int              offsets[4];
    unsigned int              hot_x, hot_y;
    struct drm_gem_object    *obj[4];
    unsigned int              filp_legacy_unused;
    ilist_node_t              head;       /* in mode_config.fb_list */
    ilist_node_t              filp_head;  /* in file fbs list */
    struct drm_framebuffer_funcs_placeholder { int dummy; } placeholder_funcs;
    int                       id;
    struct drm_file          *file;       /* file that created it (for cleanup) */
};

/* ------------------------------------------------------------------ */
/* Atomic state (forward)                                             */
/* ------------------------------------------------------------------ */

/* Per-plane state entry inside an atomic transaction. */
struct __drm_planes_state {
    struct drm_plane       *ptr;
    struct drm_plane_state *state;
    struct drm_plane_state *old_state;
    struct drm_plane_state *new_state;
    uint32_t                commit : 1;
    uint32_t                changed : 1;
};

/* Per-CRTC state entry inside an atomic transaction. */
struct __drm_crtcs_state {
    struct drm_crtc        *ptr;
    struct drm_crtc_state  *state;
    struct drm_crtc_state  *old_state;
    struct drm_crtc_state  *new_state;
    uint32_t                commit    : 1;
    uint32_t                modeset   : 1;
    uint32_t                active    : 1;
    uint32_t                planes_changed : 1;
    uint32_t                zpos_changed   : 1;
    uint32_t                connectors_changed : 1;
    uint32_t                active_changed : 1;
    uint32_t                asynchronous_commit : 1;
    uint32_t                mode_changed : 1;
};

struct drm_atomic_state {
    struct drm_device             *dev;
    uint32_t                       allow_modeset        : 1;
    uint32_t                       legacy_cursor_update : 1;
    uint32_t                       async_update         : 1;
    uint32_t                       duplicated           : 1;
    struct drm_modeset_acquire_ctx acquire_ctx;
    struct __drm_planes_state     *planes; /* array, indexed by plane index */
    struct __drm_crtcs_state      *crtcs;  /* array, indexed by crtc index */
    int                            num_connector;
    struct drm_connector         **connectors;
    struct drm_connector_state   **connector_states;
    struct drm_private_obj       **private_objs;
    struct drm_private_state     **private_states;
    int                            num_private_objs;
    void                          *commit_list; /* opaque list head */
};

/* ------------------------------------------------------------------ */
/* GEM object                                                         */
/* ------------------------------------------------------------------ */

struct drm_gem_object {
    struct drm_file          *filp_owner_default_unused;
    struct drm_device        *dev;
    int                       refcount;
    spinlock_t                ref_lock;
    uint32_t                  handle_count;
    uint32_t                  size;
    void                     *backing;          /* allocated backing memory for dumb/prime buffers */
    uint64_t                  mmap_offset;      /* offset returned by dumb_map_offset for mmap lookup */
    struct drm_mm_node        vma_node_placeholder;
    ilist_node_t              handle_list_node; /* in file objects list */
    void                     *import_attach;    /* attached dma-buf attachment (for PRIME import) */
    void                     *dma_buf;          /* dma-buf (for PRIME export) */
    int                       prime_fd;         /* assigned PRIME fd, -1 if none */
    struct drm_gem_object_funcs_placeholder { int dummy; } funcs_placeholder;
};

/* ------------------------------------------------------------------ */
/* mode_config: the registry of KMS objects                           */
/* ------------------------------------------------------------------ */

struct drm_mode_config {
    spinlock_t               mutex; /* global modeset lock */
    spinlock_t               idr_mutex;
    struct drm_idr           object_idr; /* CRTCs/connectors/encoders/planes/fbs/props */
    spinlock_t               fb_lock;
    struct drm_idr           fb_idr;
    ilist_node_t             fb_list;
    ilist_node_t             crtc_list;
    ilist_node_t             connector_list;
    ilist_node_t             encoder_list;
    ilist_node_t             plane_list;
    ilist_node_t             property_list;
    ilist_node_t             property_blob_list;
    ilist_node_t             private_obj_list;

    int                      num_connector;
    int                      num_encoder;
    int                      num_crtc;
    int                      num_plane;
    int                      num_total_plane;
    int                      num_fb;

    int                      num_connector_property_list;
    uint32_t                 min_width, min_height;
    uint32_t                 max_width, max_height;
    uint32_t                 cursor_width, cursor_height;

    struct drm_property     *prop_src_x, *prop_src_y, *prop_src_w, *prop_src_h;
    struct drm_property     *prop_crtc_x, *prop_crtc_y, *prop_crtc_w, *prop_crtc_h;
    struct drm_property     *prop_fb_id, *prop_in_fence_fd, *prop_out_fence_ptr;
    struct drm_property     *prop_crtc_id, *prop_active, *prop_mode_id;
    struct drm_property     *prop_plane_type, *prop_zpos, *prop_zpos_default;
    struct drm_property     *prop_rotation, *prop_pixel_blend_mode;
    struct drm_property     *prop_src_blend_pixel_unused;
    struct drm_property     *prop_alpha;
    struct drm_property     *prop_connector_id;
    struct drm_property     *prop_dpms, *prop_path, *prop_tile, *prop_link_status;
    struct drm_property     *prop_edid, *prop_content_protection;
    struct drm_property     *prop_scaling_mode, *prop_aspect_ratio;
    struct drm_property     *prop_vrr_capable, *prop_hdr_output_metadata;
    struct drm_property     *prop_aspect_ratio_unused;
    struct drm_property     *prop_gamma_lut, *prop_degamma_lut, *prop_ctm;
    struct drm_property     *prop_gamma_lut_size, *prop_degamma_lut_size, *prop_ctm_size;
    struct drm_property     *prop_max_bpc;
    struct drm_property     *prop_color_mode_unused;
    struct drm_property     *prop_colorspace;
    struct drm_property     *prop_writeback_fb_id, *prop_writeback_pix_fmt, *prop_writeback_out_fence_ptr;

    bool                     async_page_flip;
    bool                     fb_modifiers_not_supported;
    bool                     normalize_zpos;
    bool                     atomic_async_page_flip_not_supported_unused;
    bool                     poll_enabled;
    bool                     poll_running;
    bool                     delayed_event;
    bool                     poll_init;

    void                    *poll_work_unused;
    void                    *helper_private;
    spinlock_t               blob_lock;
};

/* ------------------------------------------------------------------ */
/* Driver                                                             */
/* ------------------------------------------------------------------ */

#define DRIVER_MODESET     BIT0_
#define DRIVER_ATOMIC      BIT1_
#define DRIVER_GEM         BIT2_
#define DRIVER_PRIME       BIT3_
#define DRIVER_RENDER      BIT4_
#define DRIVER_SYNCOBJ     BIT5_
#define DRIVER_SYNCOBJ_TIMELINE BIT6_
#define DRIVER_GEM_GPUVA   BIT7_

/* Project-internal feature bit shims (kept local to avoid polluting common.h). */
#define BIT0_ (1U << 0)
#define BIT1_ (1U << 1)
#define BIT2_ (1U << 2)
#define BIT3_ (1U << 3)
#define BIT4_ (1U << 4)
#define BIT5_ (1U << 5)
#define BIT6_ (1U << 6)
#define BIT7_ (1U << 7)

struct drm_ioctl_desc {
    unsigned int cmd;
    int (*func)(struct drm_device *dev, void *data, struct drm_file *file_priv);
    unsigned int flags;
};

/* ioctl permission flags */
#define DRM_AUTH      0x1
#define DRM_MASTER    0x2
#define DRM_ROOT_ONLY 0x4
#define DRM_UNLOCKED  0x8

struct drm_driver {
    const char             *name;
    const char             *desc;
    const char             *date;
    int                     major;
    int                     minor;
    int                     patchlevel;
    uint32_t                driver_features;

    /* /dev/dri/cardN node open/release */
    int                   (*open)(struct drm_device *dev, struct drm_file *file);
    void                 (*postclose)(struct drm_device *dev, struct drm_file *file);
    void                 (*lastclose)(struct drm_device *dev);
    void                 (*release)(struct drm_device *dev);

    /* ioctl table (NULL-terminated). */
    const struct drm_ioctl_desc *ioctls;
    int                     num_ioctls;
    int                     major_dev_unused;

    /* GEM helpers. */
    int                   (*gem_create_ioctl)(struct drm_device *dev, void *data, struct drm_file *file_priv);
    void                 (*gem_free_object)(struct drm_gem_object *obj);
    struct drm_gem_object *(*gem_prime_import)(struct drm_device *dev, void *dma_buf);

    /* PRIME export/import hooks. */
    int                   (*prime_handle_to_fd)(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle,
                                                uint32_t flags, int *prime_fd);
    int                   (*prime_fd_to_handle)(struct drm_device *dev, struct drm_file *file_priv, int prime_fd,
                                               uint32_t *handle);

    /* KMS hooks (driver-specific). */
    int                   (*mode_valid)(struct drm_device *dev, const struct drm_display_mode *mode);
    const struct drm_mode_config_funcs_placeholder { int dummy; } *mode_config_funcs_placeholder;
    void                  *fops_unused;

    /* dumb buffer callbacks. */
    int                   (*dumb_create)(struct drm_file *file_priv, struct drm_device *dev,
                                         struct drm_mode_create_dumb *args);
    int                   (*dumb_map_offset)(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle,
                                            uint64_t *offset);
    int                   (*dumb_destroy)(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle);

    int                     dev_priv_size_unused;
    uint32_t                primary_index_unused;
};

/* ------------------------------------------------------------------ */
/* File handle                                                        */
/* ------------------------------------------------------------------ */

struct drm_file {
    bool                    authenticated;
    bool                    stereo3d_allowed_unused;
    bool                    universal_planes;
    bool                    atomic;
    bool                    aspect_ratio_allowed;
    bool                    writeback_connectors_allowed_unused;
    bool                    supports_virtual_audio_unused;
    bool                    is_control_unused;

    spinlock_t              table_lock;   /* protects object_idr / GEM handle table */
    struct drm_idr          object_idr;    /* per-file object handles */

    struct drm_master      *master;        /* current master */
    struct drm_master      *is_current_unmatched_unused;
    struct drm_master      *render_master_unused;

    /* legacy magic authentication */
    spinlock_t              magic_lock;
    struct drm_open_hash    magiclist;
    drm_magic_t             magic_unused_anchor;

    ilist_node_t            head;          /* in device filelist */
    ilist_node_t            fbs_head;      /* head of framebuffer.filp_head for this file */
    ilist_node_t            object_list;   /* head of drm_gem_object.handle_list_node */

    struct drm_device      *minor_unused;
    void                   *driver_priv;
    void                   *filp_unused; /* opaque fs file pointer */

    /* client cap flags */
    uint32_t                client_caps;
    uint32_t                pad;
};

/* ------------------------------------------------------------------ */
/* Device                                                             */
/* ------------------------------------------------------------------ */

struct drm_device {
    struct drm_driver      *driver;
    void                   *dev_private;
    struct drm_mode_config  mode_config;

    spinlock_t              mutex;       /* big mode_config lock */
    spinlock_t              filelist_lock;
    ilist_node_t            filelist;    /* head of drm_file.head */

    struct drm_idr          object_idr_unused_legacy;
    spinlock_t              object_idr_lock;

    int                     open_count;
    int                     unplugged;
    bool                    vblank_disable_allowed;

    void                   *pdev_unused;       /* opaque bus device */
    void                   *busid_str;
    char                   *unique;
    int                     unique_len;

    /* vblank bookkeeping */
    struct drm_vblank_crtc  *vblank_unused_array;
    int                      num_crtc;
    spinlock_t               vblank_time_lock;
    spinlock_t               vbl_lock;

    void                   *driver_private_unused;

    /* primary node minor pointer */
    struct drm_minor        *primary;
    struct drm_minor        *render;
    struct drm_minor        *accel_unused;

    /* devtmpfs node handle (opaque) */
    void                   *dev_node_card0;
    void                   *dev_node_renderD_unused;
};

/* ------------------------------------------------------------------ */
/* Minor (per-/dev/dri node)                                          */
/* ------------------------------------------------------------------ */

struct drm_minor {
    int                     index;
    int                     type; /* 0=primary 1=render 2=accel */
    struct drm_device      *dev;
    void                   *kdev_unused;       /* opaque devtmpfs device */
    void                   *debugfs_root_unused;
    char                   *device_node_name;  /* e.g. "card0" */
};

/* ------------------------------------------------------------------ */
/* Generic / lifecycle                                                */
/* ------------------------------------------------------------------ */

/* Allocate and register a new drm_device bound to @driver. */
struct drm_device *drm_dev_alloc(struct drm_driver *driver);

/* Register the device: create devtmpfs nodes, expose KMS. */
int drm_dev_register(struct drm_device *dev, uint64_t flags);

/* Unregister and drop a reference. */
void drm_dev_unregister(struct drm_device *dev);

/* Acquire / release a reference. */
struct drm_device *drm_dev_get(struct drm_device *dev);
void drm_dev_put(struct drm_device *dev);

/* Open a /dev/dri file (called from VFS open). */
int drm_open(struct drm_device *dev, struct drm_file *file);

/* Close a /dev/dri file (called from VFS release). */
void drm_release(struct drm_file *file);

/* ioctl dispatch (called from VFS ioctl wrapper). */
int drm_ioctl(struct drm_device *dev, unsigned int cmd, void *data, struct drm_file *file_priv);

/* ioctl permission check. */
int drm_ioctl_permit(unsigned int flags, struct drm_file *file_priv);

/* Version ioctl handler. */
int drm_version(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* Dumb buffer ioctl handlers. */
int drm_gem_dumb_create(struct drm_file *file_priv, struct drm_device *dev, struct drm_mode_create_dumb *args);
int drm_gem_dumb_map_offset(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle, uint64_t *offset);
int drm_gem_dumb_destroy(struct drm_file *file_priv, struct drm_device *dev, uint32_t handle);

/* GEM ioctl handlers. */
int drm_gem_open_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_gem_close_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_gem_flink_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_gem_prime_handle_to_fd(struct drm_device *dev, struct drm_file *file_priv, uint32_t handle, uint32_t flags,
                               int *prime_fd);
int drm_gem_prime_fd_to_handle(struct drm_device *dev, struct drm_file *file_priv, int prime_fd, uint32_t *handle);
void drm_gem_prime_fd_free(int fd);

/* GEM object lifecycle. */
int drm_gem_object_init(struct drm_device *dev, struct drm_gem_object *obj, size_t size);
void drm_gem_object_get(struct drm_gem_object *obj);
void drm_gem_object_put(struct drm_gem_object *obj);
int drm_gem_handle_create(struct drm_file *file_priv, struct drm_gem_object *obj, uint32_t *handle_out);
int drm_gem_handle_delete(struct drm_file *file_priv, uint32_t handle);
struct drm_gem_object *drm_gem_object_lookup(struct drm_file *file_priv, uint32_t handle);
struct drm_gem_object *drm_gem_object_lookup_by_offset(struct drm_file *file_priv, uint64_t offset);

/* Mode object helpers (shared across KMS modules). */
int drm_mode_object_idr_alloc(struct drm_device *dev, struct drm_mode_object *obj, uint32_t type);
bool drm_mode_object_put_dec_and_test(struct drm_mode_object *obj);

/* KMS ioctl handlers. */
int drm_mode_getresources(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_setcrtc(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_cursor_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_cursor2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getconnector(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getencoder(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getplane_res(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getplane(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_setplane(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_addfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_addfb2(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_rmfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_getfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_dirtyfb(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_page_flip_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_atomic_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_wait_vblank_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* Capability ioctl handlers. */
int drm_get_cap(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_set_client_cap(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS property ioctl handlers. */
int drm_mode_getproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_obj_getproperties_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_mode_obj_setproperty_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS getfb2 handler. */
int drm_mode_getfb2_ioctl(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* Authentication ioctl handlers. */
int drm_getmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_authmagic(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_setmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);
int drm_dropmaster(struct drm_device *dev, void *data, struct drm_file *file_priv);

/* KMS object initialisation. */
int drm_crtc_init_with_planes(struct drm_device *dev, struct drm_crtc *crtc,
                              struct drm_plane *primary, struct drm_plane *cursor,
                              void *funcs, const char *name);
int drm_encoder_init(struct drm_device *dev, struct drm_encoder *encoder,
                     void *funcs, int encoder_type, const char *name);
int drm_plane_init(struct drm_device *dev, struct drm_plane *plane,
                   uint32_t possible_crtcs, void *funcs,
                   const uint32_t *formats, unsigned int format_count,
                   const uint64_t *modifiers, enum drm_plane_type type,
                   const char *name);
int drm_connector_init(struct drm_device *dev, struct drm_connector *connector,
                       void *funcs, int connector_type);
int drm_connector_attach_encoder(struct drm_connector *connector, struct drm_encoder *encoder);
int drm_connector_register(struct drm_connector *connector);
int drm_connector_update_edid_property(struct drm_connector *connector,
                                       const unsigned char *edid, size_t size);

/* drm_setversion handler. */
int drm_setversion(struct drm_device *dev, void *data, struct drm_file *file_priv);

#endif /* INCLUDE_DRM_DRM_DEVICE_H_ */
