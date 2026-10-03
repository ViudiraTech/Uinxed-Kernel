/*
 *
 *      drm_rect.h
 *      DRM rectangle helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#ifndef INCLUDE_DRM_RECT_H_
#define INCLUDE_DRM_RECT_H_

#include <libs/std/stdbool.h>
#include <libs/std/stdint.h>

#define DRM_RECT_FMT    "x%d %d %dx%d"
#define DRM_RECT_ARG(r) (r)->x1, (r)->y1, drm_rect_width(r), drm_rect_height(r)

struct drm_rect {
        int32_t x1;
        int32_t y1;
        int32_t x2;
        int32_t y2;
};

/* Width of the rectangle. */
int drm_rect_width(const struct drm_rect *r);

/* Height of the rectangle. */
int drm_rect_height(const struct drm_rect *r);

/* Initialize a rectangle at (x, y) with the given size. */
void drm_rect_init(struct drm_rect *r, int x, int y, int w, int h);

/* Grow the rectangle by (dw, dh). */
void drm_rect_adjust_size(struct drm_rect *r, int dw, int dh);

/* Shift the rectangle by (dx, dy). */
void drm_rect_translate(struct drm_rect *r, int dx, int dy);

/* Move the rectangle's top-left corner to (x, y). */
void drm_rect_translate_to(struct drm_rect *r, int x, int y);

/* Intersect r with clip in place; returns true if result is non-empty. */
bool drm_rect_intersect(struct drm_rect *r, const struct drm_rect *clip);

/* Clip src/dst by clip accounting for 16.16 scaling; returns true if visible. */
bool drm_rect_clip_scaled(struct drm_rect *src, struct drm_rect *dst, const struct drm_rect *clip);

/* Returns true if the rectangle has positive area. */
bool drm_rect_visible(const struct drm_rect *r);

#endif // INCLUDE_DRM_RECT_H_
