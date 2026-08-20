/*
 *
 *      virtgpu.c
 *      VirtIO-GPU DRM driver (full 3D / render-node / KMS)
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <arch/common.h>
#include <drivers/bus/pci.h>
#include <drivers/bus/virtpci.h>
#include <drivers/gpu/drm/drm.h>
#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_fourcc.h>
#include <drivers/gpu/drm/drm_init.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_print.h>
#include <drivers/gpu/drm/virtio/virtgpu_drv.h>
#include <drivers/gpu/drm/virtio/virtgpu_format.h>
#include <drivers/gpu/drm/virtio/virtgpu_gem.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <mem/frame.h>
#include <mem/heap.h>
#include <process/uaccess.h>

/* Ioctl implementation prototypes */

/* Probed device singleton, kept driver-private so the DRM core stays generic. */
static struct virtio_gpu_device *virtio_gpu_probed_device;

static int virtgpu_ioctl_map(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_execbuffer(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_getparam(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_create(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_info(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_transfer_from_host(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_transfer_to_host(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_wait(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_get_caps(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_resource_create_blob(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_ioctl_context_init(struct drm_device *dev, void *data, struct drm_file *file_priv);
static int virtgpu_dirty_fb(struct drm_framebuffer *fb, struct drm_file *file_priv, unsigned int flags, unsigned int color, struct drm_clip_rect *clips, unsigned int num_clips);
const struct drm_framebuffer_funcs virtgpu_fb_funcs;

/* DRM ioctl table */

static const struct drm_ioctl_desc virtgpu_ioctls[] = {
    {DRM_IOCTL_VIRTGPU_MAP,                  virtgpu_ioctl_map,                  DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_EXECBUFFER,           virtgpu_ioctl_execbuffer,           DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_GETPARAM,             virtgpu_ioctl_getparam,             0       },
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE,      virtgpu_ioctl_resource_create,      DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_RESOURCE_INFO,        virtgpu_ioctl_resource_info,        DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_TRANSFER_FROM_HOST,   virtgpu_ioctl_transfer_from_host,   DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_TRANSFER_TO_HOST,     virtgpu_ioctl_transfer_to_host,     DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_WAIT,                 virtgpu_ioctl_wait,                 DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_GET_CAPS,             virtgpu_ioctl_get_caps,             0       },
    {DRM_IOCTL_VIRTGPU_RESOURCE_CREATE_BLOB, virtgpu_ioctl_resource_create_blob, DRM_AUTH},
    {DRM_IOCTL_VIRTGPU_CONTEXT_INIT,         virtgpu_ioctl_context_init,         DRM_AUTH},
};

#define VIRTGPU_NUM_IOCTLS (sizeof(virtgpu_ioctls) / sizeof(virtgpu_ioctls[0]))

/* Driver callback implementations */

/* Open a file: allocate per-file state and assign a context id. */
static int virtgpu_open(struct drm_device *dev, struct drm_file *file)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct virtio_gpu_fpriv  *vfpriv;

    vfpriv = malloc(sizeof(*vfpriv));
    if (!vfpriv) return -ENOMEM;
    memset(vfpriv, 0, sizeof(*vfpriv));
    vfpriv->context_lock.lock = 0;

    spin_lock(&vgdev->context_idr_lock);
    vfpriv->ctx_id = vgdev->next_context_id++;
    if (!vfpriv->ctx_id) vfpriv->ctx_id = vgdev->next_context_id++;
    spin_unlock(&vgdev->context_idr_lock);

    file->driver_priv = vfpriv;
    return 0;
}

/* Lazily create the 3D context if it has not been created yet. */
static int virtgpu_ensure_context(struct virtio_gpu_device *vgdev, struct virtio_gpu_fpriv *vfpriv)
{
    int ret = 0;

    if (!vfpriv || !vgdev->has_virgl) return -EINVAL;
    spin_lock(&vfpriv->context_lock);
    if (!vfpriv->context_created) {
        uint32_t nlen = vfpriv->explicit_debug_name ? (uint32_t)strlen(vfpriv->debug_name) : 0;
        ret           = virtgpu_cmd_ctx_create(vgdev, vfpriv->ctx_id, vfpriv->context_init, vfpriv->explicit_debug_name ? vfpriv->debug_name : NULL, nlen);
        if (!ret) vfpriv->context_created = true;
    }
    spin_unlock(&vfpriv->context_lock);
    return ret;
}

/* Bind a resource to a context, tracking the attachment locally. */
int virtgpu_object_attach_context(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id)
{
    struct virtio_gpu_context_attachment *attachment;
    int                                   ret;

    if (!vgdev || !obj || !ctx_id || !obj->hw_res_handle) return -EINVAL;
    attachment = malloc(sizeof(*attachment));
    if (!attachment) return -ENOMEM;
    spin_lock(&obj->context_lock);
    for (struct virtio_gpu_context_attachment *cur = obj->context_attachments; cur; cur = cur->next) {
        if (cur->ctx_id == ctx_id) {
            spin_unlock(&obj->context_lock);
            free(attachment);
            return 0;
        }
    }
    ret = virtgpu_cmd_ctx_attach_resource(vgdev, ctx_id, obj->hw_res_handle);
    if (!ret) {
        attachment->ctx_id       = ctx_id;
        attachment->next         = obj->context_attachments;
        obj->context_attachments = attachment;
    }
    spin_unlock(&obj->context_lock);
    if (ret) free(attachment);
    return ret;
}

/* Unbind a resource from a context and drop the local attachment. */
int virtgpu_object_detach_context(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id)
{
    struct virtio_gpu_context_attachment **link;
    struct virtio_gpu_context_attachment  *attachment;
    int                                    ret;

    if (!vgdev || !obj || !ctx_id) return -EINVAL;
    spin_lock(&obj->context_lock);
    link = &obj->context_attachments;
    while (*link && (*link)->ctx_id != ctx_id) link = &(*link)->next;
    if (!*link) {
        spin_unlock(&obj->context_lock);
        return 0;
    }
    attachment = *link;
    ret        = obj->hw_res_handle ? virtgpu_cmd_ctx_detach_resource(vgdev, ctx_id, obj->hw_res_handle) : 0;
    if (!ret) *link = attachment->next;
    spin_unlock(&obj->context_lock);
    if (!ret) free(attachment);
    return ret;
}

/* Release contexts and attachments owned by this file. */
static void virtgpu_postclose(struct drm_device *dev, struct drm_file *file)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    /* Release any contexts owned by this file */

    if (file->driver_priv) {
        struct virtio_gpu_fpriv *vfpriv = (struct virtio_gpu_fpriv *)file->driver_priv;
        ilist_node_t            *node;
        for (node = file->object_list.next; node && node != &file->object_list; node = node->next) {
            struct drm_gem_handle_entry *entry = container_of(node, struct drm_gem_handle_entry, head);
            struct drm_gem_object       *gem   = entry->obj;
            struct virtio_gpu_object    *obj   = to_virtio_gpu_object(gem);
            virtgpu_object_detach_context(vgdev, obj, vfpriv->ctx_id);
        }
        if (vgdev->has_virgl && vfpriv->context_created) virtgpu_cmd_ctx_destroy(vgdev, vfpriv->ctx_id);
        free(vfpriv);
        file->driver_priv = NULL;
    }
}

/* Disable the scanout when the last client closes. */
static void virtgpu_lastclose(struct drm_device *dev)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    /* Disable scanout on last close */
    virtgpu_cmd_set_scanout(vgdev, 0, NULL);
}

/* Tear down the device and release all driver resources. */
static void virtgpu_release(struct drm_device *dev)
{
    struct virtio_gpu_device *vgdev = (struct virtio_gpu_device *)dev->dev_private;

    /* drm_dev_put() already removed the device from the core list. */
    if (vgdev) {
        for (uint32_t i = 0; i < vgdev->num_capsets; i++) free(vgdev->capsets[i].data);
        virtgpu_kms_fini(vgdev);
        drm_vblank_cleanup(dev);
        /*
         * drm_mode_config_cleanup() frees the crtc/plane/connector states
         * and unlinks every KMS object from the mode_config lists; only the
         * outer driver-allocated structs are released below.
         */
        drm_mode_config_cleanup(dev);
        if (vgdev->kms_connector) free(vgdev->kms_connector);
        if (vgdev->kms_encoder) free(vgdev->kms_encoder);
        if (vgdev->kms_crtc) free(vgdev->kms_crtc);
        if (vgdev->kms_primary) free(vgdev->kms_primary);
        virtgpu_vq_fini(vgdev);

        if (virtio_gpu_probed_device == vgdev) virtio_gpu_probed_device = NULL;
        vp_release_device(vgdev->vp_dev);
        free(vgdev->vp_dev);
        free(vgdev);
        dev->dev_private = NULL;
    }
}

/* DRM driver descriptor */

static struct drm_driver virtgpu_drm_driver = {
    .name            = "virtio_gpu",
    .desc            = "virtio GPU",
    .date            = "0",
    .major           = 0,
    .minor           = 1,
    .patchlevel      = 0,
    .driver_features = DRIVER_MODESET | DRIVER_ATOMIC | DRIVER_GEM | DRIVER_RENDER | DRIVER_SYNCHRONOUS_FLIP,

    .open      = virtgpu_open,
    .postclose = virtgpu_postclose,
    .lastclose = virtgpu_lastclose,
    .release   = virtgpu_release,

    .ioctls     = virtgpu_ioctls,
    .num_ioctls = VIRTGPU_NUM_IOCTLS,

    .gem_free_object  = virtgpu_gem_free_object,
    .gem_prime_import = virtgpu_gem_prime_import,

    .fb_funcs = &virtgpu_fb_funcs,

    .dumb_create     = virtgpu_gem_dumb_create,
    .dumb_map_offset = virtgpu_gem_dumb_map_offset,
    .dumb_destroy    = drm_gem_dumb_destroy,
};

/* KMS framebuffer damage */
static int virtgpu_dirty_fb(struct drm_framebuffer *fb, struct drm_file *file_priv, unsigned int flags, unsigned int color, struct drm_clip_rect *clips, unsigned int num_clips)
{
    struct virtio_gpu_device *vgdev;
    struct virtio_gpu_object *obj;
    struct virtio_gpu_rect    rect;
    uint32_t                  first = 0;
    uint64_t                  offset;

    (void)file_priv;
    (void)color;
    if (!fb || !fb->obj[0] || !fb->obj[0]->dev) return -EINVAL;
    vgdev = (struct virtio_gpu_device *)fb->obj[0]->dev->dev_private;
    if (!vgdev) return -EINVAL;
    obj = to_virtio_gpu_object(fb->obj[0]);

    /*
     * Off-screen buffers will be uploaded in full when they are flipped
     * onto the scanout, so avoid wasting host bandwidth here.
     */
    if (obj != vgdev->current_scanout_obj) return 0;

    if (!virtgpu_2d_formats_compatible(obj->format, fb->format)) return -EINVAL;

    if (!num_clips) {
        rect.x      = 0;
        rect.y      = 0;
        rect.width  = fb->width;
        rect.height = fb->height;
    } else {
        uint32_t x1 = fb->width, y1 = fb->height, x2 = 0, y2 = 0;

        if (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) first = 1;
        for (uint32_t i = first; i < num_clips; i += (flags & DRM_MODE_FB_DIRTY_ANNOTATE_COPY) ? 2 : 1) {
            struct drm_clip_rect *clip = &clips[i];
            if (clip->x2 > fb->width || clip->y2 > fb->height || clip->x1 > clip->x2 || clip->y1 > clip->y2) return -EINVAL;
            if (clip->x1 == clip->x2 || clip->y1 == clip->y2) continue;
            if (clip->x1 < x1) x1 = clip->x1;
            if (clip->y1 < y1) y1 = clip->y1;
            if (clip->x2 > x2) x2 = clip->x2;
            if (clip->y2 > y2) y2 = clip->y2;
        }
        if (x2 <= x1 || y2 <= y1) return 0;
        rect.x      = x1;
        rect.y      = y1;
        rect.width  = x2 - x1;
        rect.height = y2 - y1;
    }
    offset = fb->offsets[0] + (uint64_t)rect.y * obj->stride + (uint64_t)rect.x * 4;

    return virtgpu_cmd_update_2d(vgdev, obj, &rect, offset);
}

const struct drm_framebuffer_funcs virtgpu_fb_funcs = {
    .dirty = virtgpu_dirty_fb,
};

/* Ioctl: MAP */
static int virtgpu_ioctl_map(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_virtgpu_map *args = (struct drm_virtgpu_map *)data;
    struct drm_gem_object  *obj;

    (void)dev;
    obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!obj) return -ENOENT;

    if (!obj->backing) {
        drm_gem_object_put(obj);
        return -EINVAL;
    }

    if (!obj->mmap_offset) {
        drm_gem_object_put(obj);
        return -EINVAL;
    }
    args->offset = obj->mmap_offset;
    drm_gem_object_put(obj);
    return 0;
}

/* Ioctl: EXECBUFFER (3D command submission) */
static int virtgpu_ioctl_execbuffer(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device      *vgdev   = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_execbuffer *args    = (struct drm_virtgpu_execbuffer *)data;
    struct virtio_gpu_fpriv       *vfpriv  = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    uint32_t                      *handles = NULL;
    struct drm_gem_object        **bos     = NULL;
    void                          *cmd_buf = NULL;
    int                            ret;

    if (!vgdev->has_virgl) return -ENOSYS;

    if (!vfpriv) return -EINVAL;
    if (!args->size || args->size > 65536 || (args->size & 3) || !args->command) return -EINVAL;
    /*
     * Fence-fd and syncobj import/export require kernel sync-file support;
     * reject them explicitly instead of silently dropping synchronization.
     */
    if (args->flags & ~VIRTGPU_EXECBUF_FLAGS) return -EINVAL;
    if (args->flags & (VIRTGPU_EXECBUF_FENCE_FD_IN | VIRTGPU_EXECBUF_FENCE_FD_OUT)) return -EINVAL;
    if (args->syncobj_stride || args->num_in_syncobjs || args->num_out_syncobjs || args->in_syncobjs || args->out_syncobjs) return -EINVAL;
    if (args->flags & VIRTGPU_EXECBUF_RING_IDX) {
        if (!vgdev->has_context_init || !vfpriv->num_rings || args->ring_idx >= vfpriv->num_rings) return -EINVAL;
    } else if (args->ring_idx)
        return -EINVAL;
    if (virtgpu_ensure_context(vgdev, vfpriv)) return -EINVAL;
    if (args->num_bo_handles > 4096 || (!!args->num_bo_handles != !!args->bo_handles)) return -EINVAL;

    if (args->num_bo_handles) {
        handles = calloc((size_t)args->num_bo_handles, sizeof(*handles));
        bos     = calloc((size_t)args->num_bo_handles, sizeof(*bos)); // NOLINT(bugprone-sizeof-expression)
        if (!handles || !bos) {
            ret = -ENOMEM;
            goto out;
        }
        if (copy_from_user(handles, (const void *)(uintptr_t)args->bo_handles, (size_t)args->num_bo_handles * sizeof(*handles))) {
            ret = -EFAULT;
            goto out;
        }
        for (uint32_t i = 0; i < args->num_bo_handles; i++) {
            struct virtio_gpu_object *obj;
            bos[i] = drm_gem_object_lookup(file_priv, handles[i]);
            if (!bos[i]) {
                ret = -ENOENT;
                goto out;
            }
            obj = to_virtio_gpu_object(bos[i]);
            ret = virtgpu_object_attach_context(vgdev, obj, vfpriv->ctx_id);
            if (ret) goto out;
        }
    }

    cmd_buf = malloc(args->size);
    if (!cmd_buf) {
        ret = -ENOMEM;
        goto out;
    }

    /*
     * Copy command buffer from userspace (in this kernel, userspace
     * pointers are accessible directly).
     */
    if (copy_from_user(cmd_buf, (const void *)(uintptr_t)args->command, args->size)) {
        free(cmd_buf);
        cmd_buf = NULL;
        ret     = -EFAULT;
        goto out;
    }

    /* Submit to host GPU */
    ret = virtgpu_cmd_submit_3d(vgdev, vfpriv->ctx_id, args->ring_idx, !!(args->flags & VIRTGPU_EXECBUF_RING_IDX), cmd_buf, args->size, NULL);

    free(cmd_buf);
    cmd_buf = NULL;
out:
    if (cmd_buf) free(cmd_buf);
    if (bos) {
        for (uint32_t i = 0; i < args->num_bo_handles; i++)
            if (bos[i]) drm_gem_object_put(bos[i]);
    }
    free(bos);
    free(handles);
    return ret;
}

/* Ioctl: GETPARAM */
static int virtgpu_ioctl_getparam(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device    *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_getparam *args  = (struct drm_virtgpu_getparam *)data;

    int value;

    (void)file_priv;
    if (!args->value) return -EFAULT;

    switch (args->param) {
        case DRM_VIRTGPU_PARAM_3D_FEATURES :
            value = vgdev->has_virgl ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_CAPSET_QUERY_FIX :
            value = 1;
            break;
        case DRM_VIRTGPU_PARAM_RESOURCE_BLOB :
            value = vgdev->has_resource_blob ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_HOST_VISIBLE :
        case DRM_VIRTGPU_PARAM_CROSS_DEVICE :
            value = 0;
            break;
        case DRM_VIRTGPU_PARAM_EXPLICIT_DEBUG_NAME :
        case DRM_VIRTGPU_PARAM_CONTEXT_INIT :
            value = (vgdev->has_context_init && vgdev->has_virgl) ? 1 : 0;
            break;
        case DRM_VIRTGPU_PARAM_SUPPORTED_CAPSET_IDs :
            value = (int)vgdev->capset_id_mask;
            break;
        case DRM_VIRTGPU_PARAM_BLOB_ALIGNMENT :
            return -ENOENT;
            break;
        default :
            return -EINVAL;
    }
    return copy_to_user((void *)(uintptr_t)args->value, &value, sizeof(value)) ? -EFAULT : 0;
}

/* Ioctl: RESOURCE_CREATE (3D resource) */
static int virtgpu_ioctl_resource_create(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device           *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_resource_create *args  = (struct drm_virtgpu_resource_create *)data;
    struct virtio_gpu_object           *obj;
    struct virtio_gpu_fpriv            *vfpriv = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    size_t                              size;
    int                                 ret;
    uint32_t                            handle;

    if (!args->width || !args->height || !args->depth || args->bo_handle || !vfpriv) return -EINVAL;
    size = args->size ? args->size : 4096;
    if (size > UINT32_MAX) return -EINVAL;
    if (vgdev->has_virgl) {
        ret = virtgpu_ensure_context(vgdev, vfpriv);
        if (ret) return ret;
    } else {
        if (args->target != 2 || args->depth > 1 || args->array_size > 1 || args->last_level > 1 || args->nr_samples > 1) return -EINVAL;
        if (args->format != VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM && args->format != VIRTIO_GPU_FORMAT_B8G8R8X8_UNORM) return -EINVAL;
        if ((uint64_t)args->width * 4 * args->height > size) return -EINVAL;
    }

    obj = virtgpu_gem_alloc_object(dev, size);
    if (!obj) return -ENOMEM;

    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);
    obj->format        = vgdev->has_virgl ? args->format : (args->format == VIRTIO_GPU_FORMAT_B8G8R8A8_UNORM ? DRM_FORMAT_ARGB8888 : DRM_FORMAT_XRGB8888);
    obj->width         = args->width;
    obj->height        = args->height;
    obj->stride        = args->width * 4;
    obj->depth         = args->depth;
    obj->ctx_id        = vgdev->has_virgl ? vfpriv->ctx_id : 0;
    obj->created_3d    = vgdev->has_virgl;

    ret = vgdev->has_virgl ? virtgpu_cmd_create_resource_3d(vgdev, obj, args) : virtgpu_cmd_create_resource_2d(vgdev, obj);
    if (ret) {
        obj->hw_res_handle = 0;
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = virtgpu_cmd_attach_backing(vgdev, obj);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }
    obj->backing_attached = true;

    ret = drm_gem_handle_create(file_priv, &obj->base, &handle);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    args->bo_handle  = handle;
    args->res_handle = obj->hw_res_handle;
    args->size       = (uint32_t)size;
    plogk("virtgpu: 3D resource created: handle=%u, res_id=%u, %ux%ux%u fmt=0x%x\n", handle, obj->hw_res_handle, args->width, args->height, args->depth, args->format);
    drm_gem_object_put(&obj->base);
    return 0;
}

/* Ioctl: RESOURCE_INFO */
static int virtgpu_ioctl_resource_info(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_virtgpu_resource_info *args = (struct drm_virtgpu_resource_info *)data;
    struct drm_gem_object            *obj;

    (void)dev;
    obj = drm_gem_object_lookup(file_priv, args->bo_handle);
    if (!obj) return -ENOENT;

    args->size       = (uint32_t)obj->size;
    args->res_handle = ((struct virtio_gpu_object *)obj)->hw_res_handle;
    args->blob_mem   = ((struct virtio_gpu_object *)obj)->blob_mem;

    drm_gem_object_put(obj);
    return 0;
}

/* Ioctl: TRANSFER_FROM_HOST / TRANSFER_TO_HOST */
static int virtgpu_ioctl_transfer_from_host(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device       *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_3d_transfer *args  = (struct drm_virtgpu_3d_transfer *)data;
    struct drm_gem_object          *gem_obj;
    struct virtio_gpu_object       *obj;
    struct virtio_gpu_fpriv        *vfpriv = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    int                             ret;

    if (!vgdev->has_virgl || !vfpriv) return -ENOSYS;
    ret = virtgpu_ensure_context(vgdev, vfpriv);
    if (ret) return ret;

    gem_obj = drm_gem_object_lookup(file_priv, args->bo_handle);
    if (!gem_obj) return -ENOENT;

    obj = to_virtio_gpu_object(gem_obj);
    if (!obj->created_3d && !(obj->created_blob && obj->blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST)) {
        drm_gem_object_put(gem_obj);
        return -EINVAL;
    }
    if (!args->box.w || !args->box.h || !args->box.d || args->level > 31
        || (!obj->created_blob
            && (args->box.x >= (obj->width >> args->level ? obj->width >> args->level : 1) || args->box.y >= (obj->height >> args->level ? obj->height >> args->level : 1)
                || args->box.z >= (obj->depth >> args->level ? obj->depth >> args->level : 1) || args->box.w > (obj->width >> args->level ? obj->width >> args->level : 1) - args->box.x
                || args->box.h > (obj->height >> args->level ? obj->height >> args->level : 1) - args->box.y
                || args->box.d > (obj->depth >> args->level ? obj->depth >> args->level : 1) - args->box.z))) {
        drm_gem_object_put(gem_obj);
        return -EINVAL;
    }
    ret = virtgpu_object_attach_context(vgdev, obj, vfpriv->ctx_id);
    if (!ret) ret = virtgpu_cmd_transfer_3d(vgdev, obj, vfpriv->ctx_id, args, false);

    drm_gem_object_put(gem_obj);
    return ret;
}

/* DRM_VIRTGPU_TRANSFER_TO_HOST ioctl handler. */
static int virtgpu_ioctl_transfer_to_host(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device       *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_3d_transfer *args  = (struct drm_virtgpu_3d_transfer *)data;
    struct drm_gem_object          *gem_obj;
    struct virtio_gpu_object       *obj;
    struct virtio_gpu_fpriv        *vfpriv = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    int                             ret;

    gem_obj = drm_gem_object_lookup(file_priv, args->bo_handle);
    if (!gem_obj) return -ENOENT;

    obj = to_virtio_gpu_object(gem_obj);

    if (obj->created_3d || (obj->created_blob && obj->blob_mem != VIRTIO_GPU_BLOB_MEM_GUEST)) {
        if (!vgdev->has_virgl || !vfpriv) {
            drm_gem_object_put(gem_obj);
            return -EINVAL;
        }
        if (!args->box.w || !args->box.h || !args->box.d || args->level > 31 || args->box.x >= (obj->width >> args->level ? obj->width >> args->level : 1)
            || args->box.y >= (obj->height >> args->level ? obj->height >> args->level : 1) || args->box.z >= (obj->depth >> args->level ? obj->depth >> args->level : 1)
            || args->box.w > (obj->width >> args->level ? obj->width >> args->level : 1) - args->box.x || args->box.h > (obj->height >> args->level ? obj->height >> args->level : 1) - args->box.y
            || args->box.d > (obj->depth >> args->level ? obj->depth >> args->level : 1) - args->box.z) {
            drm_gem_object_put(gem_obj);
            return -EINVAL;
        }
        ret = virtgpu_ensure_context(vgdev, vfpriv);
        if (!ret) ret = virtgpu_object_attach_context(vgdev, obj, vfpriv->ctx_id);
        if (!ret) ret = virtgpu_cmd_transfer_3d(vgdev, obj, vfpriv->ctx_id, args, true);
    } else if (obj->created_blob) {
        ret = -EINVAL;
    } else {
        ret = virtgpu_cmd_transfer_to_host_2d_rect(vgdev, obj, args);
    }

    drm_gem_object_put(gem_obj);
    return ret;
}

/* Ioctl: WAIT (fence wait) */
static int virtgpu_ioctl_wait(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct drm_virtgpu_3d_wait *args = (struct drm_virtgpu_3d_wait *)data;
    struct drm_gem_object      *gem_obj;

    (void)dev;

    if (args->flags & ~VIRTGPU_WAIT_NOWAIT) return -EINVAL;
    gem_obj = drm_gem_object_lookup(file_priv, args->handle);
    if (!gem_obj) return -ENOENT;

    /*
     * Control/3D submissions are fenced and completed synchronously by this
     * driver, so a successfully looked-up BO is never still busy here.
     */
    drm_gem_object_put(gem_obj);
    return 0;
}

/* Ioctl: GET_CAPS */
static int virtgpu_ioctl_get_caps(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device       *vgdev     = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_get_caps    *args      = (struct drm_virtgpu_get_caps *)data;
    void                           *caps_data = NULL;
    void                           *user_copy = NULL;
    uint32_t                        max_size  = 0, copy_size;
    struct virtio_gpu_capset_cache *cache     = NULL;
    bool                            found     = false;
    int                             ret;

    (void)file_priv;

    if (!vgdev->num_capsets) return -ENOSYS;
    if (!args->addr || !args->size || args->pad) return -EINVAL;

    for (uint32_t i = 0; i < vgdev->num_capsets; i++) {
        if (vgdev->capsets[i].id == args->cap_set_id && args->cap_set_ver <= vgdev->capsets[i].max_version) {
            cache    = &vgdev->capsets[i];
            max_size = cache->max_size;
            found    = true;
            break;
        }
    }
    if (!found || !max_size) return -EINVAL;
    copy_size = args->size < max_size ? args->size : max_size;

    spin_lock(&vgdev->capset_lock);
    if (cache->data && cache->cached_version == args->cap_set_ver) {
        caps_data = cache->data;
    } else {
        caps_data = malloc(max_size);
        if (!caps_data) {
            spin_unlock(&vgdev->capset_lock);
            return -ENOMEM;
        }
        ret = virtgpu_cmd_get_capset(vgdev, args->cap_set_id, args->cap_set_ver, caps_data, max_size);
        if (ret) {
            free(caps_data);
            spin_unlock(&vgdev->capset_lock);
            return ret;
        }
        free(cache->data);
        cache->data           = caps_data;
        cache->cached_version = args->cap_set_ver;
    }

    user_copy = malloc(copy_size);
    if (!user_copy) {
        spin_unlock(&vgdev->capset_lock);
        return -ENOMEM;
    }
    memcpy(user_copy, caps_data, copy_size);
    spin_unlock(&vgdev->capset_lock);
    ret = copy_to_user((void *)(uintptr_t)args->addr, user_copy, copy_size) ? -EFAULT : 0;
    free(user_copy);
    return ret;
}

/* Ioctl: RESOURCE_CREATE_BLOB */
static int virtgpu_ioctl_resource_create_blob(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device                *vgdev = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_resource_create_blob *args  = (struct drm_virtgpu_resource_create_blob *)data;
    struct virtio_gpu_object                *obj;
    struct virtio_gpu_fpriv                 *vfpriv = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    bool                                     guest_blob, host3d_blob;
    void                                    *cmd_buf = NULL;
    int                                      ret;
    uint32_t                                 handle;

    if (!vgdev->has_resource_blob) return -EINVAL;
    if (!args->size || args->size > UINT32_MAX || args->pad || args->pad2 || args->bo_handle || !vfpriv) return -EINVAL;
    if (args->blob_flags & ~(VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE | VIRTIO_GPU_BLOB_FLAG_USE_SHAREABLE | VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE)) return -EINVAL;
    if (args->blob_hints & ~DRM_VIRTGPU_BLOB_FLAG_HINT_DEFER_MAPPING) return -EINVAL;
    if (args->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_CROSS_DEVICE) return -EINVAL;

    guest_blob  = args->blob_mem == VIRTIO_GPU_BLOB_MEM_GUEST || args->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    host3d_blob = args->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D || args->blob_mem == VIRTIO_GPU_BLOB_MEM_HOST3D_GUEST;
    if (!guest_blob && !host3d_blob) return -EINVAL;
    if (host3d_blob) {
        if (!vgdev->has_virgl || (args->cmd_size & 3) || args->cmd_size > 65536 || (args->cmd_size && !args->cmd)) return -EINVAL;
        ret = virtgpu_ensure_context(vgdev, vfpriv);
        if (ret) return ret;
        if (args->cmd_size) {
            cmd_buf = malloc(args->cmd_size);
            if (!cmd_buf) return -ENOMEM;
            if (copy_from_user(cmd_buf, (const void *)(uintptr_t)args->cmd, args->cmd_size)) {
                free(cmd_buf);
                return -EFAULT;
            }
            ret = virtgpu_cmd_submit_3d(vgdev, vfpriv->ctx_id, 0, false, cmd_buf, args->cmd_size, NULL);
            free(cmd_buf);
            if (ret) return ret;
        }
    } else if (args->blob_id || args->cmd_size || args->cmd)
        return -EINVAL;
    if (!guest_blob && (args->blob_flags & VIRTIO_GPU_BLOB_FLAG_USE_MAPPABLE)) return -EINVAL;

    obj = virtgpu_gem_alloc_object(dev, args->size);
    if (!obj) return -ENOMEM;

    obj->hw_res_handle = virtgpu_resource_id_alloc(vgdev);
    obj->created_blob  = true;
    obj->blob_mem      = args->blob_mem;
    obj->blob_flags    = args->blob_flags;
    obj->blob_id       = args->blob_id;
    obj->ctx_id        = host3d_blob ? vfpriv->ctx_id : 0;
    obj->width         = (uint32_t)args->size;
    obj->height        = 1;
    obj->format        = 0;
    obj->depth         = 1;

    if (!guest_blob) {
        free(obj->entries);
        if (obj->backing_phys) free_frames(obj->backing_phys, obj->backing_page_count);
        obj->entries            = NULL;
        obj->base.backing       = NULL;
        obj->num_entries        = 0;
        obj->backing_phys       = 0;
        obj->backing_page_count = 0;
    }

    ret = virtgpu_cmd_create_blob(vgdev, obj, args);
    if (ret) {
        obj->hw_res_handle = 0;
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    ret = drm_gem_handle_create(file_priv, &obj->base, &handle);
    if (ret) {
        virtgpu_gem_free_object(&obj->base);
        return ret;
    }

    args->bo_handle  = handle;
    args->res_handle = obj->hw_res_handle;
    plogk("virtgpu: Blob resource created: handle=%u, res_id=%u, size=%llu, mem=%u flags=0x%x\n", handle, obj->hw_res_handle, args->size, args->blob_mem, args->blob_flags);
    drm_gem_object_put(&obj->base);
    return 0;
}

/* Ioctl: CONTEXT_INIT */
static int virtgpu_ioctl_context_init(struct drm_device *dev, void *data, struct drm_file *file_priv)
{
    struct virtio_gpu_device            *vgdev  = (struct virtio_gpu_device *)dev->dev_private;
    struct drm_virtgpu_context_init     *args   = (struct drm_virtgpu_context_init *)data;
    struct virtio_gpu_fpriv             *vfpriv = (struct virtio_gpu_fpriv *)file_priv->driver_priv;
    struct drm_virtgpu_context_set_param params[4];
    uint32_t                             context_init = 0, num_rings = 0;
    uint64_t                             ring_mask = 0;
    char                                 debug_name[VIRTGPU_DEBUG_NAME_MAX];
    bool                                 seen_capset = false, seen_rings = false, seen_mask = false, seen_name = false;
    int                                  ret;

    if (!vgdev->has_virgl || !vgdev->has_context_init) return -EINVAL;
    if (!vfpriv || args->pad || args->num_params > 4 || (args->num_params && !args->ctx_set_params)) return -EINVAL;
    memset(params, 0, sizeof(params));
    memset(debug_name, 0, sizeof(debug_name));
    if (args->num_params && copy_from_user(params, (const void *)(uintptr_t)args->ctx_set_params, args->num_params * sizeof(params[0]))) return -EFAULT;

    for (uint32_t i = 0; i < args->num_params; i++) {
        switch (params[i].param) {
            case VIRTGPU_CONTEXT_PARAM_CAPSET_ID :
                if (seen_capset || params[i].value >= 64) return -EINVAL;
                if (params[i].value && !(vgdev->capset_id_mask & (1ULL << params[i].value))) return -EINVAL;
                seen_capset  = true;
                context_init = (uint32_t)params[i].value;
                break;
            case VIRTGPU_CONTEXT_PARAM_NUM_RINGS :
                if (seen_rings || !params[i].value || params[i].value > VIRTGPU_MAX_CONTEXT_RINGS) return -EINVAL;
                seen_rings = true;
                num_rings  = (uint32_t)params[i].value;
                break;
            case VIRTGPU_CONTEXT_PARAM_POLL_RINGS_MASK :
                if (seen_mask) return -EINVAL;
                seen_mask = true;
                ring_mask = params[i].value;
                break;
            case VIRTGPU_CONTEXT_PARAM_DEBUG_NAME : {
                int n;
                if (seen_name || !params[i].value) return -EINVAL;
                n = strncpy_from_user(debug_name, (const char *)(uintptr_t)params[i].value, sizeof(debug_name));
                if (n < 0) return -EFAULT;
                if ((uint32_t)n >= sizeof(debug_name)) return -EINVAL;
                seen_name = true;
                break;
            }
            default :
                return -EINVAL;
        }
    }
    if (ring_mask && (!num_rings || (num_rings < 64 && (ring_mask >> num_rings)))) return -EINVAL;

    spin_lock(&vfpriv->context_lock);
    if (vfpriv->context_created) {
        spin_unlock(&vfpriv->context_lock);
        return -EEXIST;
    }
    vfpriv->context_init        = context_init;
    vfpriv->num_rings           = num_rings;
    vfpriv->ring_idx_mask       = ring_mask;
    vfpriv->explicit_debug_name = seen_name;
    if (seen_name) memcpy(vfpriv->debug_name, debug_name, sizeof(debug_name));
    ret = virtgpu_cmd_ctx_create(vgdev, vfpriv->ctx_id, context_init, seen_name ? vfpriv->debug_name : NULL, seen_name ? (uint32_t)strlen(vfpriv->debug_name) : 0);
    if (!ret) vfpriv->context_created = true;
    spin_unlock(&vfpriv->context_lock);
    if (ret) return ret;

    plogk("virtgpu: Context created: ctx_id=%u capset=%u rings=%u\n", vfpriv->ctx_id, context_init, num_rings);
    return 0;
}

/* Page-flip: switch scanout to a new framebuffer (called from KMS) */
int virtgpu_page_flip(struct virtio_gpu_device *vgdev, struct drm_framebuffer *fb, struct drm_framebuffer *old_fb)
{
    struct drm_gem_object    *gem_obj    = fb ? fb->obj[0] : NULL;
    struct virtio_gpu_object *obj        = NULL;
    int                       scanout_id = 0;
    int                       ret;

    (void)old_fb;

    if (gem_obj) {
        if (!vgdev) return -EINVAL;
        obj = to_virtio_gpu_object(gem_obj);

        if (!obj) return -EINVAL;

        if (obj->created_blob) {
            obj->width  = fb->width;
            obj->height = fb->height;
            obj->format = fb->format;
            obj->stride = fb->pitches[0];
            ret         = virtgpu_cmd_set_scanout_blob(vgdev, scanout_id, obj);
            if (ret) return ret;
            vgdev->current_scanout_obj = obj;
            vgdev->current_fb          = fb;
            return 0;
        }

        /*
         * A 2D resource has an implicit packed row stride.  Userspace may
         * expose a smaller top-left framebuffer view of an over-allocated
         * resource, but the view must fit and preserve the resource layout.
         */
        if (fb->width > obj->width || fb->height > obj->height || fb->pitches[0] != obj->stride || fb->offsets[0] || !virtgpu_2d_formats_compatible(obj->format, fb->format)) return -EINVAL;

        /*
         * Submit the full flip as one ordered batch and avoid rebinding an
         * object that is already the active scanout.
         */
        bool layout_changed = !vgdev->current_fb || vgdev->current_fb->width != fb->width || vgdev->current_fb->height != fb->height || vgdev->current_fb->pitches[0] != fb->pitches[0]
                              || vgdev->current_fb->offsets[0] != fb->offsets[0];
        ret = virtgpu_cmd_update_scanout_2d(vgdev, scanout_id, obj, fb->width, fb->height, obj != vgdev->current_scanout_obj || old_fb == NULL || layout_changed);
        if (ret) {
            DRM_ERROR("Flip: batched update failed: %d\n", ret);
            return ret;
        }

        vgdev->current_scanout_obj = obj;
    } else {
        /* Disable scanout */
        ret = virtgpu_cmd_set_scanout(vgdev, scanout_id, NULL);
        if (ret) return ret;
        vgdev->current_scanout_obj = NULL;
    }

    vgdev->current_fb = fb;
    return 0;
}

/* DebugFS - simple feature dump */
static void virtgpu_debugfs_info(struct virtio_gpu_device *vgdev)
{
    plogk("virtgpu: Virgl=%d, edid=%d, blob=%d, ctx_init=%d\n", vgdev->has_virgl, vgdev->has_edid, vgdev->has_resource_blob, vgdev->has_context_init);
    plogk("virtgpu: %d scanout(s), ctrlq %d, cursorq %d\n", vgdev->num_scanouts, vgdev->ctrlq.num_max, vgdev->cursorq.num_max);
}

/* Module init / probe */
int virtio_gpu_driver_init(void)
{
    struct virtio_gpu_device *vgdev;
    struct vp_device         *vp;
    uint64_t                  features;
    struct virtio_gpu_config  gpu_config;
    int                       ret;

    vp = malloc(sizeof(*vp));
    if (!vp) {
        plogk("virtgpu: out of memory allocating virtio-pci device state.\n");
        return -ENOMEM;
    }

    ret = vp_find_device(PCI_VENDOR_ID_REDHAT, PCI_DEVICE_ID_VIRTIO_GPU, vp);
    if (ret) {
        free(vp);
        return ret;
    }

    vp_setup_device(vp);

    /* Negotiate features */
    features = (1ULL << VIRTIO_GPU_F_VIRGL) | (1ULL << VIRTIO_GPU_F_EDID) | (1ULL << VIRTIO_GPU_F_RESOURCE_UUID) | (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB) | (1ULL << VIRTIO_GPU_F_CONTEXT_INIT);

    ret = vp_negotiate_features(vp, features, &features);
    if (ret) {
        vp_release_device(vp);
        free(vp);
        return ret;
    }

    /* Allocate the virtio-gpu device */
    vgdev = malloc(sizeof(*vgdev));
    if (!vgdev) {
        plogk("virtgpu: out of memory allocating GPU device state.\n");
        vp_release_device(vp);
        free(vp);
        return -ENOMEM;
    }
    memset(vgdev, 0, sizeof(*vgdev));

    vgdev->vp_dev    = vp;
    vp->private_data = vgdev;

    vgdev->has_virgl         = !!(features & (1ULL << VIRTIO_GPU_F_VIRGL));
    vgdev->has_edid          = !!(features & (1ULL << VIRTIO_GPU_F_EDID));
    vgdev->has_resource_blob = !!(features & (1ULL << VIRTIO_GPU_F_RESOURCE_BLOB));
    vgdev->has_context_init  = vgdev->has_virgl && !!(features & (1ULL << VIRTIO_GPU_F_CONTEXT_INIT));

    vgdev->resource_idr_lock.lock = 0;
    vgdev->context_idr_lock.lock  = 0;
    vgdev->ctrlq_cmd_busy         = 0;
    vgdev->cursorq_cmd_busy       = 0;
    wait_queue_init(&vgdev->ctrlq_cmd_wait);
    wait_queue_init(&vgdev->cursorq_cmd_wait);
    vgdev->fence_lock.lock  = 0;
    vgdev->next_resource_id = 1;
    vgdev->next_context_id  = 1;
    vgdev->next_fence_id    = 1;
    vgdev->num_scanouts     = 0;
    vgdev->capset_lock.lock = 0;

    /*
     * VirtIO spec 3.1.1: step 5 - set FEATURES_OK and verify.
     * DRIVER_OK must be set LAST, after all virtqueues are configured.
     */
    vp_set_status(vp, VIRTIO_STATUS_ACKNOWLEDGE | VIRTIO_STATUS_DRIVER | VIRTIO_STATUS_FEATURES_OK);
    compiler_barrier();
    {
        uint8_t s = vp_get_status(vp);
        if (!(s & VIRTIO_STATUS_FEATURES_OK)) {
            plogk("virtgpu: Device rejected feature negotiation (status=0x%02x)\n", s);
            vp_release_device(vp);
            free(vgdev);
            return -ENODEV;
        }
    }

    /* Set up virtqueues (must be done before DRIVER_OK) */
    ret = virtgpu_vq_init(vgdev);
    if (ret) {
        vp_release_device(vp);
        free(vgdev);
        return ret;
    }

    /*
     * VirtIO spec 3.1.1: step 8 - set DRIVER_OK after queues are ready.
     * Use a write barrier to ensure all virtqueue setup stores are visible
     * to the device before the status write reaches it.
     */
    compiler_barrier();
    vp_set_status(vp, vp_get_status(vp) | VIRTIO_STATUS_DRIVER_OK);
    compiler_barrier();

    memset(&gpu_config, 0, sizeof(gpu_config));
    vp_read_device_config(vp, &gpu_config, 0, sizeof(gpu_config));
    vgdev->num_scanouts = gpu_config.num_scanouts > 16 ? 16 : (int)gpu_config.num_scanouts;

    /*
     * capset_index is 32-bit, but an untrusted device must not force an
     * unbounded probe loop.  The advertised UAPI mask has 64 bits.
     */
    vgdev->num_capsets = gpu_config.num_capsets > 64 ? 64 : gpu_config.num_capsets;
    for (uint32_t i = 0; i < vgdev->num_capsets; i++) {
        uint32_t id, version, size;
        if (!virtgpu_cmd_get_capset_info(vgdev, i, &id, &version, &size)) {
            vgdev->capsets[i].id          = id;
            vgdev->capsets[i].max_version = version;
            vgdev->capsets[i].max_size    = size;
            if (id < 64) vgdev->capset_id_mask |= 1ULL << id;
        }
    }

    /* Allocate the DRM device that carries driver-private state. */
    vgdev->drm_dev = drm_dev_alloc(&virtgpu_drm_driver);
    if (!vgdev->drm_dev) {
        DRM_ERROR("failed to allocate DRM device.\n");
        virtgpu_vq_fini(vgdev);
        vp_release_device(vp);
        free(vgdev);
        return -ENOMEM;
    }

    vgdev->drm_dev->dev_private = vgdev;

    /*
     * Initialise the GPU first (display info, EDID, KMS pipeline and the
     * initial modeset) and only then publish the device to the DRM core,
     * like Linux: the boot log shows the virtio-gpu info before the
     * "drm: Initialized virtio_gpu" banner.
     */
    ret = virtgpu_kms_init(vgdev);
    if (ret) {
        DRM_ERROR("KMS init failed: %d (continuing with render only)\n", ret);
        /* Non-fatal - render node still works */
    }

    /* Debug info */
    virtgpu_debugfs_info(vgdev);

    /* Publishes the /dev/dri nodes, sysfs and the core device list. */
    ret = drm_dev_register(vgdev->drm_dev, 0);
    if (ret) {
        /* drm_dev_unregister() releases every KMS/vq/pci resource. */
        drm_dev_unregister(vgdev->drm_dev);
        return ret;
    }

    /* drm_dev_register() already published the device to the DRM core. */
    virtio_gpu_probed_device = vgdev;

    plogk("virtgpu: VirtIO GPU driver registered (card/render node, 3D=%d, blob=%d)\n", vgdev->has_virgl, vgdev->has_resource_blob);

    return 0;
}

/* Initialisation hook - called from kernel init */

/*
 * GPU probe callback, registered as "virtio_gpu" with the GPU driver bus
 * and driven by gpu_drivers_probe().  Returns 0 if a device was attached.
 */
int virtio_gpu_probe(void)
{
#if CONFIG_VIRTIO_GPU
    return virtio_gpu_driver_init();
#else
    return -ENODEV;
#endif
}

/*
 * Return the probed virtio-gpu device, or NULL if no device was found.
 * This is driver-private state; the DRM core itself never needs it.
 */
void *virtio_gpu_get_device(void)
{
    return virtio_gpu_probed_device;
}

/* Module exit hook. */
void virtio_gpu_module_exit(void)
{
    /* No-op: device lifecycle is managed by the DRM release callback. */
}
