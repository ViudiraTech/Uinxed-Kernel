/*
 *
 *      drm_blend.c
 *      DRM plane blending helpers
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
#include <drivers/drm/drm_fourcc.h>
#include <drivers/drm/drm_idr.h>
#include <drivers/drm/drm_mode.h>
#include <drivers/drm/drm_modeset_lock.h>
#include <drivers/drm/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/*
 * drm_plane_create_zpos_property - Set the default z-position for a plane.
 * @plane: plane
 * @zpos: default z-position value
 *
 * Stores the default zpos in the plane's zpos_property_default field.
 * In a full implementation this would also create a DRM range property
 * and attach it; MVP stores the value only.
 */
void drm_plane_create_zpos_property(struct drm_plane *plane, unsigned int zpos)
{
    if (!plane) { return; }

    plane->zpos_property_default = zpos;
}

/*
 * drm_plane_create_rotation_property - Create the rotation property for a plane.
 * @plane: plane
 * @rotation: bitmask of supported rotation/reflection flags
 *
 * MVP stub: the property infrastructure is not yet wired.
 * Returns 0.
 */
int drm_plane_create_rotation_property(struct drm_plane *plane, unsigned int rotation)
{
    if (!plane) { return -EINVAL; }

    (void)rotation;

    /* MVP stub: property creation deferred to drm_property_create_bitmask */
    return 0;
}

/*
 * drm_plane_create_blend_mode_property - Create the pixel blend mode property.
 * @plane: plane
 * @blend_mode: bitmask of supported blend modes
 *
 * MVP stub: the property infrastructure is not yet wired.
 * Returns 0.
 */
int drm_plane_create_blend_mode_property(struct drm_plane *plane, unsigned int blend_mode)
{
    if (!plane) { return -EINVAL; }

    (void)blend_mode;

    /* MVP stub: property creation deferred to drm_property_create_enum */
    return 0;
}

/*
 * drm_plane_create_alpha_property - Create the alpha property for a plane.
 * @plane: plane
 *
 * MVP stub: the property infrastructure is not yet wired.
 * Returns 0.
 */
int drm_plane_create_alpha_property(struct drm_plane *plane)
{
    if (!plane) { return -EINVAL; }

    /* MVP stub: property creation deferred to drm_property_create_range */
    return 0;
}