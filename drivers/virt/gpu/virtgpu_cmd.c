/*
 *
 *      virtgpu_cmd.c
 *      VirtIO-GPU command encoding
 *
 *      2026/7/23 By JiTianYu391
 *      Copyright Â© 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 *  Encodes and submits individual virtio-gpu protocol commands.  Each
 *  function allocates a command buffer, fills in the protocol struct,
 *  sends it via virtgpu_ctrl_cmd, and unpacks the response.
 *
 */

#include <drivers/virt/virtgpu_drv.h>
#include <drivers/virt/virtgpu_vq.h>
#include <mem/alloc.h>

/* ------------------------------------------------------------------ */
/* Display information                                                 */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_display_info(struct virtio_gpu_device *vgdev)
{
    struct virtio_gpu_ctrl_hdr          cmd;
    struct virtio_gpu_resp_display_info resp;
    int                                 i, ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = VIRTIO_GPU_CMD_GET_DISPLAY_INFO;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    vgdev->num_scanouts = 0;
    for (i = 0; i < 16; i++) {
        struct virtio_gpu_display_one *d = &resp.pmodes[i];
        if (!d->enabled) { continue; }
        vgdev->scanouts[vgdev->num_scanouts].width   = d->r.width;
        vgdev->scanouts[vgdev->num_scanouts].height  = d->r.height;
        vgdev->scanouts[vgdev->num_scanouts].enabled = true;
        vgdev->scanouts[vgdev->num_scanouts].rect    = d->r;
        vgdev->num_scanouts++;
    }

    DRM_INFO("Display info: %d scanout(s)\n", vgdev->num_scanouts);
    for (i = 0; i < vgdev->num_scanouts && i < 16; i++) {
        DRM_INFO("  scanout[%d]: %ux%u\n", i, vgdev->scanouts[i].width, vgdev->scanouts[i].height);
    }
    return 0;
}

/* ------------------------------------------------------------------ */
/* EDID                                                                */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_edid(struct virtio_gpu_device *vgdev, int scanout_id, void *edid, int *edid_size)
{
    struct virtio_gpu_get_edid  cmd;
    struct virtio_gpu_resp_edid resp;
    int                         ret;

    if (!vgdev->has_edid) { return -ENOSYS; }

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_GET_EDID;
    cmd.scanout_id = scanout_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    if (resp.size > 0 && resp.size <= 1024) {
        memcpy(edid, resp.edid, resp.size);
        *edid_size = resp.size;
    } else {
        *edid_size = 0;
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* Resource creation â€?2D                                              */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_resource_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj)
{
    struct virtio_gpu_create_resource_2d cmd;
    struct virtio_gpu_ctrl_hdr           resp;
    int                                  ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_2D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.format      = virtgpu_drm_format_to_virtio(obj->format);
    if (!cmd.format) return -EINVAL;
    cmd.width  = obj->width;
    cmd.height = obj->height;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource creation â€?3D                                              */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_resource_3d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct drm_virtgpu_resource_create *rc)
{
    struct virtio_gpu_resource_create_3d cmd;
    struct virtio_gpu_ctrl_hdr           resp;
    int                                  ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_3D;
    cmd.hdr.ctx_id  = obj->ctx_id;
    cmd.resource_id = obj->hw_res_handle;
    cmd.target      = rc->target;
    /* RESOURCE_CREATE uses virgl's host format namespace, not DRM fourcc. */
    cmd.format     = rc->format;
    cmd.bind       = rc->bind;
    cmd.width      = rc->width;
    cmd.height     = rc->height;
    cmd.depth      = rc->depth;
    cmd.array_size = rc->array_size;
    cmd.last_level = rc->last_level;
    cmd.nr_samples = rc->nr_samples;
    cmd.flags      = rc->flags;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource creation â€?Blob                                           */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_create_blob(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct drm_virtgpu_resource_create_blob *blob)
{
    struct virtio_gpu_resource_create_blob *cmd;
    struct virtio_gpu_ctrl_hdr              resp;
    size_t                                  cmd_size;
    int                                     ret;

    if (!vgdev || !obj || !blob || (!!obj->num_entries != !!obj->entries)) return -EINVAL;
    cmd_size = sizeof(*cmd) + (size_t)obj->num_entries * sizeof(*obj->entries);
    cmd      = malloc(cmd_size);
    if (!cmd) return -ENOMEM;
    memset(cmd, 0, cmd_size);
    cmd->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_CREATE_BLOB;
    cmd->hdr.ctx_id  = obj->ctx_id;
    cmd->resource_id = obj->hw_res_handle;
    cmd->blob_mem    = blob->blob_mem;
    cmd->blob_flags  = blob->blob_flags;
    cmd->nr_entries  = obj->num_entries;
    cmd->blob_id     = blob->blob_id;
    cmd->size        = blob->size;
    if (obj->num_entries) memcpy(cmd + 1, obj->entries, obj->num_entries * sizeof(*obj->entries));

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, cmd, (int)cmd_size, &resp, sizeof(resp), NULL);
    free(cmd);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource destruction                                                */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_unref_resource(struct virtio_gpu_device *vgdev, uint32_t res_id)
{
    struct virtio_gpu_unref    cmd;
    struct virtio_gpu_ctrl_hdr resp;
    int                        ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_UNREF;
    cmd.resource_id = res_id;
    cmd.padding     = 0;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Backing management (guest memory attach/detach)                     */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_attach_backing(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj)
{
    struct virtio_gpu_resource_attach_backing *cmd;
    struct virtio_gpu_ctrl_hdr                 resp;
    struct virtio_gpu_mem_entry               *entries;
    int                                        ret, cmd_size;

    if (!obj->num_entries || !obj->entries) { return -EINVAL; }

    cmd_size = sizeof(*cmd) + obj->num_entries * sizeof(struct virtio_gpu_mem_entry);
    cmd      = malloc(cmd_size);
    if (!cmd) { return -ENOMEM; }

    memset(cmd, 0, cmd_size);
    cmd->hdr.type    = VIRTIO_GPU_CMD_RESOURCE_ATTACH_BACKING;
    cmd->resource_id = obj->hw_res_handle;
    cmd->nr_entries  = obj->num_entries;

    entries = (struct virtio_gpu_mem_entry *)(cmd + 1);
    memcpy(entries, obj->entries, obj->num_entries * sizeof(struct virtio_gpu_mem_entry));

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, cmd, cmd_size, &resp, sizeof(resp), NULL);

    free(cmd);
    return ret;
}

int virtgpu_cmd_detach_backing(struct virtio_gpu_device *vgdev, uint32_t res_id)
{
    struct virtio_gpu_resource_attach_backing cmd;
    struct virtio_gpu_ctrl_hdr                resp;
    int                                       ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_DETACH_BACKING;
    cmd.resource_id = res_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Data transfer (2D)                                                  */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_transfer_to_host_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint64_t offset)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    struct virtio_gpu_ctrl_hdr            resp;
    int                                   ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.offset      = offset;
    cmd.r.x         = 0;
    cmd.r.y         = 0;
    cmd.r.width     = obj->width;
    cmd.r.height    = obj->height;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_transfer_to_host_2d_rect(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj,
                                         const struct drm_virtgpu_3d_transfer *xf)
{
    struct virtio_gpu_transfer_to_host_2d cmd;
    struct virtio_gpu_ctrl_hdr            resp;

    if (!vgdev || !obj || !xf || !xf->box.w || !xf->box.h || xf->box.z || xf->box.d > 1 || xf->box.x >= obj->width || xf->box.y >= obj->height
        || xf->box.w > obj->width - xf->box.x || xf->box.h > obj->height - xf->box.y || xf->level || xf->stride || xf->layer_stride)
        return -EINVAL;
    if ((uint64_t)xf->offset + (uint64_t)(xf->box.h - 1) * obj->stride + (uint64_t)xf->box.w * 4 > obj->base.size) return -EINVAL;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    cmd.resource_id = obj->hw_res_handle;
    cmd.offset      = xf->offset;
    cmd.r.x         = xf->box.x;
    cmd.r.y         = xf->box.y;
    cmd.r.width     = xf->box.w;
    cmd.r.height    = xf->box.h;
    memset(&resp, 0, sizeof(resp));
    return virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
}

/* Transfer and flush one damage rectangle with a single queue kick. */
int virtgpu_cmd_update_2d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, const struct virtio_gpu_rect *rect, uint64_t offset)
{
    struct virtio_gpu_transfer_to_host_2d transfer;
    struct virtio_gpu_resource_flush      flush;
    struct virtio_gpu_ctrl_hdr            responses[2];
    struct virtgpu_vq_command             commands[2];
    struct virtio_gpu_rect                damage;

    if (!vgdev || !obj) return -EINVAL;

    if (rect) {
        damage = *rect;
    } else {
        damage.x      = 0;
        damage.y      = 0;
        damage.width  = obj->width;
        damage.height = obj->height;
    }
    if (!damage.width || !damage.height || damage.x >= obj->width || damage.y >= obj->height) return 0;
    if (damage.width > obj->width - damage.x) damage.width = obj->width - damage.x;
    if (damage.height > obj->height - damage.y) damage.height = obj->height - damage.y;

    memset(&transfer, 0, sizeof(transfer));
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r           = damage;
    transfer.offset      = offset;
    transfer.resource_id = obj->hw_res_handle;

    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r           = damage;
    flush.resource_id = obj->hw_res_handle;

    memset(responses, 0, sizeof(responses));
    commands[0] = (struct virtgpu_vq_command) {&transfer, sizeof(transfer), &responses[0], sizeof(responses[0])};
    commands[1] = (struct virtgpu_vq_command) {&flush, sizeof(flush), &responses[1], sizeof(responses[1])};
    return virtgpu_ctrl_cmd_batch(vgdev, commands, 2);
}

/* Full-frame update used by page flips.  A new resource needs
 * TRANSFER -> SET_SCANOUT -> FLUSH ordering; an already-bound resource
 * skips the redundant SET_SCANOUT command. */
int virtgpu_cmd_update_scanout_2d(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj, bool set_scanout)
{
    struct virtio_gpu_transfer_to_host_2d transfer;
    struct virtio_gpu_set_scanout         scanout;
    struct virtio_gpu_resource_flush      flush;
    struct virtio_gpu_ctrl_hdr            responses[3];
    struct virtgpu_vq_command             commands[3];
    uint32_t                              count = 0;

    if (!vgdev || !obj) return -EINVAL;

    memset(&transfer, 0, sizeof(transfer));
    transfer.hdr.type    = VIRTIO_GPU_CMD_TRANSFER_TO_HOST_2D;
    transfer.r.width     = obj->width;
    transfer.r.height    = obj->height;
    transfer.resource_id = obj->hw_res_handle;

    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    scanout.r.width     = obj->width;
    scanout.r.height    = obj->height;
    scanout.scanout_id  = scanout_id;
    scanout.resource_id = obj->hw_res_handle;

    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r.width     = obj->width;
    flush.r.height    = obj->height;
    flush.resource_id = obj->hw_res_handle;

    memset(responses, 0, sizeof(responses));
    commands[count] = (struct virtgpu_vq_command) {&transfer, sizeof(transfer), &responses[count], sizeof(responses[count])};
    count++;
    if (set_scanout) {
        commands[count] = (struct virtgpu_vq_command) {&scanout, sizeof(scanout), &responses[count], sizeof(responses[count])};
        count++;
    }
    commands[count] = (struct virtgpu_vq_command) {&flush, sizeof(flush), &responses[count], sizeof(responses[count])};
    count++;

    return virtgpu_ctrl_cmd_batch(vgdev, commands, count);
}

/* ------------------------------------------------------------------ */
/* Data transfer (3D)                                                  */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_transfer_3d(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, uint32_t ctx_id,
                            const struct drm_virtgpu_3d_transfer *xf, bool to_host)
{
    struct virtio_gpu_transfer_3d cmd;
    struct virtio_gpu_ctrl_hdr    resp;
    int                           ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type     = to_host ? VIRTIO_GPU_CMD_TRANSFER_TO_HOST_3D : VIRTIO_GPU_CMD_TRANSFER_FROM_HOST_3D;
    cmd.hdr.ctx_id   = ctx_id;
    cmd.resource_id  = obj->hw_res_handle;
    cmd.offset       = xf->offset;
    cmd.level        = xf->level;
    cmd.stride       = xf->stride;
    cmd.layer_stride = xf->layer_stride;
    cmd.box.x        = xf->box.x;
    cmd.box.y        = xf->box.y;
    cmd.box.z        = xf->box.z;
    cmd.box.w        = xf->box.w;
    cmd.box.h        = xf->box.h;
    cmd.box.d        = xf->box.d;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Resource flush                                                      */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_resource_flush(struct virtio_gpu_device *vgdev, struct virtio_gpu_object *obj, struct virtio_gpu_rect *rect)
{
    struct virtio_gpu_resource_flush cmd;
    struct virtio_gpu_ctrl_hdr       resp;
    int                              ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    cmd.resource_id = obj->hw_res_handle;
    if (rect) {
        cmd.r = *rect;
    } else {
        cmd.r.x      = 0;
        cmd.r.y      = 0;
        cmd.r.width  = obj->width;
        cmd.r.height = obj->height;
    }

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Scanout                                                             */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_set_scanout(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj)
{
    struct virtio_gpu_set_scanout cmd;
    struct virtio_gpu_ctrl_hdr    resp;
    int                           ret;
    struct virtio_gpu_rect        r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT;
    cmd.scanout_id  = scanout_id;
    cmd.resource_id = obj ? obj->hw_res_handle : 0;

    if (obj) {
        r.x      = 0;
        r.y      = 0;
        r.width  = obj->width;
        r.height = obj->height;
    } else {
        memset(&r, 0, sizeof(r));
    }
    cmd.r = r;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_set_scanout_blob(struct virtio_gpu_device *vgdev, int scanout_id, struct virtio_gpu_object *obj)
{
    struct virtio_gpu_set_scanout_blob scanout;
    struct virtio_gpu_resource_flush   flush;
    struct virtio_gpu_ctrl_hdr         responses[2];
    struct virtgpu_vq_command          commands[2];

    if (!vgdev || !obj || !obj->created_blob || !obj->width || !obj->height || !obj->stride) return -EINVAL;
    memset(&scanout, 0, sizeof(scanout));
    scanout.hdr.type    = VIRTIO_GPU_CMD_SET_SCANOUT_BLOB;
    scanout.r.width     = obj->width;
    scanout.r.height    = obj->height;
    scanout.scanout_id  = (uint32_t)scanout_id;
    scanout.resource_id = obj->hw_res_handle;
    scanout.width       = obj->width;
    scanout.height      = obj->height;
    scanout.format      = virtgpu_drm_format_to_virtio(obj->format);
    if (!scanout.format) return -EINVAL;
    scanout.strides[0] = obj->stride;

    memset(&flush, 0, sizeof(flush));
    flush.hdr.type    = VIRTIO_GPU_CMD_RESOURCE_FLUSH;
    flush.r           = scanout.r;
    flush.resource_id = obj->hw_res_handle;
    memset(responses, 0, sizeof(responses));
    commands[0] = (struct virtgpu_vq_command) {&scanout, sizeof(scanout), &responses[0], sizeof(responses[0])};
    commands[1] = (struct virtgpu_vq_command) {&flush, sizeof(flush), &responses[1], sizeof(responses[1])};
    return virtgpu_ctrl_cmd_batch(vgdev, commands, 2);
}

/* ------------------------------------------------------------------ */
/* Context management (3D)                                             */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_ctx_create(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t context_init, const char *debug_name, uint32_t name_len)
{
    struct virtio_gpu_ctx_create cmd;
    struct virtio_gpu_ctrl_hdr   resp;
    int                          ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_CTX_CREATE;
    cmd.hdr.ctx_id = ctx_id;
    if (name_len >= sizeof(cmd.debug_name)) return -EINVAL;
    cmd.nlen         = name_len;
    cmd.context_init = context_init;
    if (name_len && debug_name) memcpy(cmd.debug_name, debug_name, name_len);

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_ctx_destroy(struct virtio_gpu_device *vgdev, uint32_t ctx_id)
{
    struct virtio_gpu_ctx_destroy cmd;
    struct virtio_gpu_ctrl_hdr    resp;
    int                           ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type   = VIRTIO_GPU_CMD_CTX_DESTROY;
    cmd.hdr.ctx_id = ctx_id;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    return ret;
}

int virtgpu_cmd_update_cursor(struct virtio_gpu_device *vgdev, uint32_t scanout_id, struct virtio_gpu_object *obj, int32_t x, int32_t y,
                              int32_t hot_x, int32_t hot_y)
{
    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_UPDATE_CURSOR;
    cmd.pos.scanout_id = scanout_id;
    cmd.pos.x          = (uint32_t)x;
    cmd.pos.y          = (uint32_t)y;
    cmd.resource_id    = obj ? obj->hw_res_handle : 0;
    cmd.hot_x          = (uint32_t)hot_x;
    cmd.hot_y          = (uint32_t)hot_y;
    return virtgpu_cursor_cmd(vgdev, &cmd, sizeof(cmd));
}

int virtgpu_cmd_move_cursor(struct virtio_gpu_device *vgdev, uint32_t scanout_id, int32_t x, int32_t y)
{
    struct virtio_gpu_update_cursor cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_MOVE_CURSOR;
    cmd.pos.scanout_id = scanout_id;
    cmd.pos.x          = (uint32_t)x;
    cmd.pos.y          = (uint32_t)y;
    return virtgpu_cursor_cmd(vgdev, &cmd, sizeof(cmd));
}

static int virtgpu_cmd_ctx_resource(struct virtio_gpu_device *vgdev, uint32_t type, uint32_t ctx_id, uint32_t resource_id)
{
    struct virtio_gpu_ctx_resource cmd;
    struct virtio_gpu_ctrl_hdr     resp;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type    = type;
    cmd.hdr.ctx_id  = ctx_id;
    cmd.resource_id = resource_id;
    memset(&resp, 0, sizeof(resp));
    return virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
}

int virtgpu_cmd_ctx_attach_resource(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t resource_id)
{
    return virtgpu_cmd_ctx_resource(vgdev, VIRTIO_GPU_CMD_CTX_ATTACH_RESOURCE, ctx_id, resource_id);
}

int virtgpu_cmd_ctx_detach_resource(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t resource_id)
{
    return virtgpu_cmd_ctx_resource(vgdev, VIRTIO_GPU_CMD_CTX_DETACH_RESOURCE, ctx_id, resource_id);
}

/* ------------------------------------------------------------------ */
/* 3D command submission                                               */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_submit_3d(struct virtio_gpu_device *vgdev, uint32_t ctx_id, uint32_t ring_idx, bool use_ring_idx, const void *cmd_data,
                          uint32_t size, struct virtio_gpu_fence *fence)
{
    struct virtio_gpu_submit_3d *cmd;
    struct virtio_gpu_ctrl_hdr   resp;
    int                          ret;

    cmd = malloc(sizeof(*cmd) + size);
    if (!cmd) { return -ENOMEM; }

    memset(cmd, 0, sizeof(*cmd));
    cmd->hdr.type     = VIRTIO_GPU_CMD_SUBMIT_3D;
    cmd->hdr.ctx_id   = ctx_id;
    cmd->hdr.fence_id = fence ? fence->id : 0;
    if (use_ring_idx) {
        cmd->hdr.flags |= VIRTIO_GPU_FLAG_INFO_RING_IDX;
        cmd->hdr.ring_idx = (uint8_t)ring_idx;
    }
    cmd->size    = size;
    cmd->padding = 0;

    memcpy((uint8_t *)(cmd + 1), cmd_data, size);

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, cmd, sizeof(*cmd) + size, &resp, sizeof(resp), NULL);

    free(cmd);
    return ret;
}

/* ------------------------------------------------------------------ */
/* Capset query                                                        */
/* ------------------------------------------------------------------ */

int virtgpu_cmd_get_capset_info(struct virtio_gpu_device *vgdev, uint32_t idx, uint32_t *capset_id, uint32_t *max_version, uint32_t *max_size)
{
    struct virtio_gpu_get_capset_info  cmd;
    struct virtio_gpu_resp_capset_info resp;
    int                                ret;

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type     = VIRTIO_GPU_CMD_GET_CAPSET_INFO;
    cmd.capset_index = idx;

    memset(&resp, 0, sizeof(resp));
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), &resp, sizeof(resp), NULL);
    if (ret) { return ret; }

    *capset_id   = resp.capset_id;
    *max_version = resp.capset_max_version;
    *max_size    = resp.capset_max_size;
    return 0;
}

int virtgpu_cmd_get_capset(struct virtio_gpu_device *vgdev, uint32_t capset_id, uint32_t version, void *data, uint32_t max_size)
{
    struct virtio_gpu_get_capset   cmd;
    struct virtio_gpu_resp_capset *resp;
    int                            ret, resp_size;

    resp_size = sizeof(struct virtio_gpu_resp_capset) + max_size;
    resp      = malloc(resp_size);
    if (!resp) { return -ENOMEM; }

    memset(&cmd, 0, sizeof(cmd));
    cmd.hdr.type       = VIRTIO_GPU_CMD_GET_CAPSET;
    cmd.capset_id      = capset_id;
    cmd.capset_version = version;

    memset(resp, 0, resp_size);
    ret = virtgpu_ctrl_cmd(vgdev, &cmd, sizeof(cmd), resp, resp_size, NULL);
    if (ret) {
        free(resp);
        return ret;
    }

    memcpy(data, resp->capset_data, max_size);
    free(resp);
    return 0;
}
