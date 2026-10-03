/*
 *
 *      drm_rect.c
 *      DRM rectangle helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_rect.h>

#if CONFIG_DRM

/* Width of the rectangle. */
int drm_rect_width(const struct drm_rect *r)
{
    return r->x2 - r->x1;
}

/* Height of the rectangle. */
int drm_rect_height(const struct drm_rect *r)
{
    return r->y2 - r->y1;
}

/* Initialize a rectangle at (x, y) with the given size. */
void drm_rect_init(struct drm_rect *r, int x, int y, int w, int h)
{
    r->x1 = x;
    r->y1 = y;
    r->x2 = x + w;
    r->y2 = y + h;
}

/* Grow the rectangle by (dw, dh). */
void drm_rect_adjust_size(struct drm_rect *r, int dw, int dh)
{
    r->x2 += dw;
    r->y2 += dh;
}

/* Shift the rectangle by (dx, dy). */
void drm_rect_translate(struct drm_rect *r, int dx, int dy)
{
    r->x1 += dx;
    r->y1 += dy;
    r->x2 += dx;
    r->y2 += dy;
}

/* Move the rectangle's top-left corner to (x, y). */
void drm_rect_translate_to(struct drm_rect *r, int x, int y)
{
    r->x2 += x - r->x1;
    r->y2 += y - r->y1;
    r->x1 = x;
    r->y1 = y;
}

/* Returns true if the rectangle has strictly positive area. */
bool drm_rect_visible(const struct drm_rect *r)
{
    return (r->x2 > r->x1) && (r->y2 > r->y1);
}

/* Intersect @r with @clip in place; returns true if the resulting rectangle is visible. */
bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *clip)
{
    int32_t x1, y1, x2, y2;

    x1 = (r->x1 > clip->x1) ? r->x1 : clip->x1;
    y1 = (r->y1 > clip->y1) ? r->y1 : clip->y1;
    x2 = (r->x2 < clip->x2) ? r->x2 : clip->x2;
    y2 = (r->y2 < clip->y2) ? r->y2 : clip->y2;

    r->x1 = x1;
    r->y1 = y1;
    r->x2 = x2;
    r->y2 = y2;

    return drm_rect_visible(r);
}

/*
 * Clip @src and @dst against @clip while preserving the 16.16 fixed-point
 * scaling ratio between source and destination. All scaling arithmetic is
 * performed in 64 bits to avoid overflow. Returns true if the destination
 * rectangle remains visible after clipping.
 */
bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst, const struct drm_rect *clip)
{
    int64_t diff;

    /* A degenerate destination cannot be clipped; avoid division by zero. */
    if (drm_rect_width(dst) == 0 || drm_rect_height(dst) == 0) return drm_rect_visible(dst);

    diff = (int64_t)clip->x1 - (int64_t)dst->x1;
    if (diff > 0) {
        int64_t src_diff = diff * (int64_t)drm_rect_width(src) / (int64_t)drm_rect_width(dst);
        if (src_diff < 0) src_diff = 0;
        src->x1 = (int32_t)((int64_t)src->x1 + src_diff);
        dst->x1 = clip->x1;
    }

    diff = (int64_t)clip->x2 - (int64_t)dst->x2;
    if (diff < 0) {
        int64_t src_diff = diff * (int64_t)drm_rect_width(src) / (int64_t)drm_rect_width(dst);
        if (src_diff > 0) src_diff = 0;
        src->x2 = (int32_t)((int64_t)src->x2 + src_diff);
        dst->x2 = clip->x2;
    }

    diff = (int64_t)clip->y1 - (int64_t)dst->y1;
    if (diff > 0) {
        int64_t src_diff = diff * (int64_t)drm_rect_height(src) / (int64_t)drm_rect_height(dst);
        if (src_diff < 0) src_diff = 0;
        src->y1 = (int32_t)((int64_t)src->y1 + src_diff);
        dst->y1 = clip->y1;
    }

    diff = (int64_t)clip->y2 - (int64_t)dst->y2;
    if (diff < 0) {
        int64_t src_diff = diff * (int64_t)drm_rect_height(src) / (int64_t)drm_rect_height(dst);
        if (src_diff > 0) src_diff = 0;
        src->y2 = (int32_t)((int64_t)src->y2 + src_diff);
        dst->y2 = clip->y2;
    }

    return drm_rect_visible(dst);
}

#endif
