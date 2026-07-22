/*
 *
 *      drm_atomic_helper.c
 *      DRM atomic helper functions
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/drm/drm_device.h>
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

/* ------------------------------------------------------------------ */
/* Locally-defined state structs (forward-declared in drm_device.h)   */
/* ------------------------------------------------------------------ */
/* Helper: container_of                                                */
/* ------------------------------------------------------------------ */

#define container_of(ptr, type, member) ((type *)((char *)(ptr) - offsetof(type, member)))

/* ------------------------------------------------------------------ */
/* Forward declarations for cross-file calls                           */
/* ------------------------------------------------------------------ */

extern struct drm_crtc_state      *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern struct drm_plane_state     *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane);
extern struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector);
extern int                         drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern int                         drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc);
extern void                        drm_atomic_state_free(struct drm_atomic_state *state);

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_check_modeset: check CRTC mode changes            */
/* ------------------------------------------------------------------ */

int drm_atomic_helper_check_modeset(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    /* Check each CRTC for mode / active changes */
    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state) { continue; }

        /* Detect mode changes */
        if (crtc_state->active_changed) { crtc_state->mode_changed = true; }

        if (crtc_state->mode_changed) { DRM_DEBUG_KMS("CRTC %d: mode changed\n", i); }

        if (crtc_state->active_changed) { DRM_DEBUG_KMS("CRTC %d: active changed to %s\n", i, crtc_state->active ? "on" : "off"); }
    }

    /* Check each connector for CRTC changes */
    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];
        struct drm_connector       *connector  = state->connectors[i];

        if (!conn_state || !connector || !connector->state) { continue; }

        if (conn_state->crtc != connector->state->crtc) {
            struct drm_crtc *old_crtc = connector->state->crtc;
            struct drm_crtc *new_crtc = conn_state->crtc;

            conn_state->crtc_changed = true;

            /* Mark connectors_changed on old CRTC */
            if (old_crtc) {
                struct drm_crtc_state *old_crtc_state;

                old_crtc_state = drm_atomic_get_crtc_state(state, old_crtc);
                if (old_crtc_state) { old_crtc_state->connectors_changed = true; }
            }

            /* Mark connectors_changed on new CRTC */
            if (new_crtc) {
                struct drm_crtc_state *new_crtc_state;

                new_crtc_state = drm_atomic_get_crtc_state(state, new_crtc);
                if (new_crtc_state) { new_crtc_state->connectors_changed = true; }
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_check_planes: validate plane fb format            */
/* ------------------------------------------------------------------ */

int drm_atomic_helper_check_planes(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    if (!state->planes) { return 0; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *plane_entry = &state->planes[i];
        struct drm_plane_state    *plane_state = plane_entry->state;

        if (!plane_state) { continue; }

        /* Check fb format compatibility */
        if (plane_state->fb) {
            struct drm_plane *plane = plane_entry->ptr;
            unsigned int      j;
            bool              format_ok = false;

            if (!plane || !plane->format_types || plane->format_count == 0) {
                DRM_ERROR("Plane %d: no format list\n", i);
                return -EINVAL;
            }

            for (j = 0; j < plane->format_count; j++) {
                if (plane->format_types[j] == plane_state->fb->format) {
                    format_ok = true;
                    break;
                }
            }

            if (!format_ok) {
                DRM_ERROR("Plane %d: format 0x%x not supported\n", i, plane_state->fb->format);
                return -EINVAL;
            }
        }

        /* Determine visibility */
        if (plane_state->fb && plane_state->crtc) {
            plane_state->visible = true;
        } else {
            plane_state->visible = false;
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_commit_modeset_disables: disable CRTCs            */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_commit_modeset_disables(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state || !crtc_entry->ptr) { continue; }

        /* Disable CRTC if active is changing to false */
        if (crtc_state->active_changed && !crtc_state->active) {
            crtc_entry->ptr->enabled = false;
            DRM_DEBUG_KMS("CRTC %d: disabled\n", i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_commit_modeset_enables: enable CRTCs, set mode    */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_commit_modeset_enables(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state || !crtc_entry->ptr) { continue; }

        /* Enable CRTC if active is changing to true */
        if (crtc_state->active_changed && crtc_state->active) {
            crtc_entry->ptr->enabled = true;

            /* Set mode if mode changed */
            if (crtc_state->mode_changed) { memcpy(&crtc_entry->ptr->mode, &crtc_state->mode, sizeof(crtc_state->mode)); }

            DRM_DEBUG_KMS("CRTC %d: enabled\n", i);
        }
    }
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_commit_planes: apply plane fb and coordinates     */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_commit_planes(struct drm_device *dev, struct drm_atomic_state *state, uint32_t flags)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    (void)flags;

    if (!state->planes) { return; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *plane_entry = &state->planes[i];
        struct drm_plane_state    *plane_state = plane_entry->state;

        if (!plane_state || !plane_entry->ptr) { continue; }

        /* Apply new framebuffer if changed */
        if (plane_state->fb) {
            struct drm_plane *plane = plane_entry->ptr;

            if (plane->state) {
                plane->state->fb      = plane_state->fb;
                plane->state->crtc    = plane_state->crtc;
                plane->state->src     = plane_state->src;
                plane->state->dst     = plane_state->dst;
                plane->state->visible = plane_state->visible;
            }
        }
    }
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_setup_commit: stub                                 */
/* ------------------------------------------------------------------ */

int drm_atomic_helper_setup_commit(struct drm_atomic_state *state, bool nonblocking)
{
    (void)state;
    (void)nonblocking;
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_wait_for_vblanks: stub (no vblank yet)            */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_wait_for_vblanks(struct drm_device *dev, struct drm_atomic_state *state)
{
    (void)dev;
    (void)state;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_wait_for_flip_done: stub                           */
/* ------------------------------------------------------------------ */

int drm_atomic_helper_wait_for_flip_done(struct drm_device *dev, struct drm_atomic_state *state)
{
    (void)dev;
    (void)state;
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_cleanup_planes: free old plane states, put old fbs */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_cleanup_planes(struct drm_device *dev, struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    (void)dev;

    if (!state->planes) { return; }

    for (i = 0; i < config->num_total_plane; i++) {
        struct __drm_planes_state *plane_entry = &state->planes[i];

        /* Free old state if present */
        if (plane_entry->old_state) {
            free(plane_entry->old_state);
            plane_entry->old_state = NULL;
        }

        /* Free new state if present and not yet committed */
        if (plane_entry->new_state) {
            free(plane_entry->new_state);
            plane_entry->new_state = NULL;
        }
    }
}

/* ------------------------------------------------------------------ */
/* drm_atomic_helper_commit_tail: default commit tail sequence         */
/* ------------------------------------------------------------------ */

void drm_atomic_helper_commit_tail(struct drm_atomic_state *state)
{
    struct drm_device *dev = state->dev;

    drm_atomic_helper_commit_modeset_disables(dev, state);
    drm_atomic_helper_commit_planes(dev, state, 0);
    drm_atomic_helper_commit_modeset_enables(dev, state);
    drm_atomic_helper_wait_for_vblanks(dev, state);
    drm_atomic_helper_cleanup_planes(dev, state);
}