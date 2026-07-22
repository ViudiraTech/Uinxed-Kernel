/*
 *
 *      drm_atomic.c
 *      DRM atomic modesetting core
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
/* drm_atomic_state_alloc: allocate and initialize an atomic state     */
/* ------------------------------------------------------------------ */

struct drm_atomic_state *drm_atomic_state_alloc(struct drm_device *dev)
{
    struct drm_atomic_state *state;
    struct drm_mode_config  *config = &dev->mode_config;

    state = malloc(sizeof(*state));
    if (!state) { return NULL; }
    memset(state, 0, sizeof(*state));
    state->dev = dev;

    drm_modeset_acquire_init(&state->acquire_ctx, 0);

    /* Allocate per-plane state array */
    if (config->num_total_plane > 0) {
        state->planes = malloc(sizeof(*state->planes) * config->num_total_plane);
        if (!state->planes) {
            free(state);
            return NULL;
        }
        memset(state->planes, 0, sizeof(*state->planes) * config->num_total_plane);
    }

    /* Allocate per-CRTC state array */
    if (config->num_crtc > 0) {
        state->crtcs = malloc(sizeof(*state->crtcs) * config->num_crtc);
        if (!state->crtcs) {
            free(state->planes);
            free(state);
            return NULL;
        }
        memset(state->crtcs, 0, sizeof(*state->crtcs) * config->num_crtc);
    }

    return state;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_state_default_clear: free all sub-state allocations      */
/* ------------------------------------------------------------------ */

void drm_atomic_state_default_clear(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    /* Free per-plane substates */
    if (state->planes) {
        for (i = 0; i < config->num_total_plane; i++) {
            if (state->planes[i].state) {
                free(state->planes[i].state);
                state->planes[i].state = NULL;
            }
            if (state->planes[i].old_state) {
                free(state->planes[i].old_state);
                state->planes[i].old_state = NULL;
            }
            if (state->planes[i].new_state) {
                free(state->planes[i].new_state);
                state->planes[i].new_state = NULL;
            }
        }
        free(state->planes);
        state->planes = NULL;
    }

    /* Free per-CRTC substates */
    if (state->crtcs) {
        for (i = 0; i < config->num_crtc; i++) {
            if (state->crtcs[i].state) {
                free(state->crtcs[i].state);
                state->crtcs[i].state = NULL;
            }
            if (state->crtcs[i].old_state) {
                free(state->crtcs[i].old_state);
                state->crtcs[i].old_state = NULL;
            }
            if (state->crtcs[i].new_state) {
                free(state->crtcs[i].new_state);
                state->crtcs[i].new_state = NULL;
            }
        }
        free(state->crtcs);
        state->crtcs = NULL;
    }

    /* Free connector substates */
    if (state->connector_states) {
        for (i = 0; i < state->num_connector; i++) {
            if (state->connector_states[i]) {
                free(state->connector_states[i]);
                state->connector_states[i] = NULL;
            }
        }
        free(state->connector_states);
        state->connector_states = NULL;
    }

    /* Free connector pointer array */
    if (state->connectors) {
        free(state->connectors);
        state->connectors = NULL;
    }

    state->num_connector = 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_state_clear: reset state after default clear             */
/* ------------------------------------------------------------------ */

void drm_atomic_state_clear(struct drm_atomic_state *state)
{
    drm_atomic_state_default_clear(state);
    state->allow_modeset        = 0;
    state->legacy_cursor_update = 0;
    state->async_update         = 0;
    state->duplicated           = 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_state_free: fully release an atomic state                */
/* ------------------------------------------------------------------ */

void drm_atomic_state_free(struct drm_atomic_state *state)
{
    if (!state) { return; }
    drm_atomic_state_default_clear(state);
    drm_modeset_acquire_fini(&state->acquire_ctx);
    free(state);
}

/* ------------------------------------------------------------------ */
/* drm_atomic_get_crtc_state: get or create CRTC state for @crtc       */
/* ------------------------------------------------------------------ */

struct drm_crtc_state *drm_atomic_get_crtc_state(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct __drm_crtcs_state *crtc_entry = &state->crtcs[crtc->index];

    if (crtc_entry->state) { return crtc_entry->state; }

    /* Allocate new CRTC state */
    crtc_entry->state = malloc(sizeof(*crtc_entry->state));
    if (!crtc_entry->state) { return NULL; }
    memset(crtc_entry->state, 0, sizeof(*crtc_entry->state));
    crtc_entry->state->crtc = crtc;
    crtc_entry->ptr         = crtc;

    /* Copy from existing CRTC state if available */
    if (crtc->state) { memcpy(crtc_entry->state, crtc->state, sizeof(*crtc_entry->state)); }

    return crtc_entry->state;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_get_plane_state: get or create plane state for @plane    */
/* ------------------------------------------------------------------ */

struct drm_plane_state *drm_atomic_get_plane_state(struct drm_atomic_state *state, struct drm_plane *plane)
{
    struct drm_mode_config    *config = &state->dev->mode_config;
    struct __drm_planes_state *plane_entry;
    int                        idx = -1;

    /* Find the plane's index in the plane_list */
    {
        ilist_node_t *node;
        int           i = 0;
        for (node = config->plane_list.next; node != &config->plane_list; node = node->next, i++) {
            struct drm_plane *p = container_of(node, struct drm_plane, head);
            if (p == plane) {
                idx = i;
                break;
            }
        }
    }

    if (idx < 0 || idx >= config->num_total_plane) { return NULL; }

    plane_entry = &state->planes[idx];

    if (plane_entry->state) { return plane_entry->state; }

    /* Allocate new plane state */
    plane_entry->state = malloc(sizeof(*plane_entry->state));
    if (!plane_entry->state) { return NULL; }
    memset(plane_entry->state, 0, sizeof(*plane_entry->state));
    plane_entry->state->plane = plane;
    plane_entry->ptr          = plane;

    /* Copy from existing plane state if available */
    if (plane->state) { memcpy(plane_entry->state, plane->state, sizeof(*plane_entry->state)); }

    return plane_entry->state;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_get_connector_state: get or create connector state       */
/* ------------------------------------------------------------------ */

struct drm_connector_state *drm_atomic_get_connector_state(struct drm_atomic_state *state, struct drm_connector *connector)
{
    int i;

    /* Check if connector already in state */
    for (i = 0; i < state->num_connector; i++) {
        if (state->connectors[i] == connector) { return state->connector_states[i]; }
    }

    /* Grow arrays */
    {
        size_t                       new_count = state->num_connector + 1;
        struct drm_connector       **new_connectors;
        struct drm_connector_state **new_states;

        new_connectors = realloc(state->connectors, sizeof(*new_connectors) * new_count);
        if (!new_connectors) { return NULL; }

        new_states = realloc(state->connector_states, sizeof(*new_states) * new_count);
        if (!new_states) {
            /* realloc for connectors succeeded but states failed;
             * revert connectors to old size (realloc with old size). */
            state->connectors = realloc(new_connectors, sizeof(*new_connectors) * state->num_connector);
            return NULL;
        }

        state->connectors                       = new_connectors;
        state->connector_states                 = new_states;
        state->connectors[state->num_connector] = connector;

        /* Allocate new connector state */
        state->connector_states[state->num_connector] = malloc(sizeof(*state->connector_states[0]));
        if (!state->connector_states[state->num_connector]) { return NULL; }
        memset(state->connector_states[state->num_connector], 0, sizeof(*state->connector_states[0]));
        state->connector_states[state->num_connector]->connector = connector;

        /* Copy from existing connector state if available */
        if (connector->state) { memcpy(state->connector_states[state->num_connector], connector->state, sizeof(*state->connector_states[0])); }

        state->num_connector++;
        return state->connector_states[state->num_connector - 1];
    }
}

/* ------------------------------------------------------------------ */
/* drm_atomic_add_affected_planes: add all planes on @crtc to state    */
/* ------------------------------------------------------------------ */

int drm_atomic_add_affected_planes(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    ilist_node_t           *node;
    uint32_t                crtc_mask = 1U << crtc->index;

    for (node = config->plane_list.next; node != &config->plane_list; node = node->next) {
        struct drm_plane *plane = container_of(node, struct drm_plane, head);

        if (plane->possible_crtcs & crtc_mask) {
            struct drm_plane_state *plane_state;

            plane_state = drm_atomic_get_plane_state(state, plane);
            if (!plane_state) { return -ENOMEM; }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_add_affected_connectors: add all connectors on @crtc     */
/* ------------------------------------------------------------------ */

int drm_atomic_add_affected_connectors(struct drm_atomic_state *state, struct drm_crtc *crtc)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    ilist_node_t           *node;

    (void)crtc;

    for (node = config->connector_list.next; node != &config->connector_list; node = node->next) {
        struct drm_connector       *connector = container_of(node, struct drm_connector, head);
        struct drm_connector_state *conn_state;

        conn_state = drm_atomic_get_connector_state(state, connector);
        if (!conn_state) { return -ENOMEM; }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_check_only: validate the atomic state                    */
/* ------------------------------------------------------------------ */

int drm_atomic_check_only(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    /* Validate CRTC states */
    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state) { continue; }

        /* If active, a mode must be set */
        if (crtc_state->active) {
            if (crtc_state->mode.clock == 0 && crtc_state->mode.hdisplay == 0) {
                DRM_ERROR("CRTC %d: active but no mode set\n", i);
                return -EINVAL;
            }
        }
    }

    /* Validate plane states */
    if (state->planes) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct __drm_planes_state *plane_entry = &state->planes[i];
            struct drm_plane_state    *plane_state = plane_entry->state;

            if (!plane_state) { continue; }

            /* If plane has a framebuffer, validate format */
            if (plane_state->fb) {
                unsigned int j;
                bool         format_ok = false;

                if (!plane_state->plane->format_types || plane_state->plane->format_count == 0) {
                    DRM_ERROR("Plane %d: fb set but no format list\n", i);
                    return -EINVAL;
                }

                for (j = 0; j < plane_state->plane->format_count; j++) {
                    if (plane_state->plane->format_types[j] == plane_state->fb->format) {
                        format_ok = true;
                        break;
                    }
                }

                if (!format_ok) {
                    DRM_ERROR("Plane %d: incompatible fb format\n", i);
                    return -EINVAL;
                }
            }

            /* If plane has a CRTC, it must be valid */
            if (plane_state->crtc) {
                if (plane_state->crtc->index >= config->num_crtc) {
                    DRM_ERROR("Plane %d: invalid CRTC index\n", i);
                    return -EINVAL;
                }
            }
        }
    }

    /* Validate connector states */
    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];

        if (!conn_state) { continue; }

        /* If connector has a CRTC, it must be valid */
        if (conn_state->crtc) {
            if (conn_state->crtc->index >= config->num_crtc) {
                DRM_ERROR("Connector %d: invalid CRTC\n", i);
                return -EINVAL;
            }
        }
    }

    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_commit: validate and apply the atomic state              */
/* ------------------------------------------------------------------ */

int drm_atomic_commit(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     ret;
    int                     i;

    ret = drm_atomic_check_only(state);
    if (ret < 0) { return ret; }

    /* Apply CRTC states: enable/disable and set mode */
    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state || !crtc_entry->ptr) { continue; }

        if (crtc_state->active_changed) { crtc_entry->ptr->enabled = crtc_state->active; }

        if (crtc_state->mode_changed && crtc_state->active) { memcpy(&crtc_entry->ptr->mode, &crtc_state->mode, sizeof(crtc_state->mode)); }
    }

    /* Apply plane states: framebuffer and coordinates */
    if (state->planes) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct __drm_planes_state *plane_entry = &state->planes[i];
            struct drm_plane_state    *plane_state = plane_entry->state;

            if (!plane_state || !plane_entry->ptr) { continue; }

            /* Apply framebuffer */
            if (plane_state->fb) {
                if (plane_entry->ptr->state) {
                    plane_entry->ptr->state->fb               = plane_state->fb;
                    plane_entry->ptr->state->crtc             = plane_state->crtc;
                    plane_entry->ptr->state->src              = plane_state->src;
                    plane_entry->ptr->state->dst              = plane_state->dst;
                    plane_entry->ptr->state->visible          = plane_state->visible;
                    plane_entry->ptr->state->rotation         = plane_state->rotation;
                    plane_entry->ptr->state->alpha            = plane_state->alpha;
                    plane_entry->ptr->state->zpos             = plane_state->zpos;
                    plane_entry->ptr->state->pixel_blend_mode = plane_state->pixel_blend_mode;
                }
            }
        }
    }

    /* Apply connector states: CRTC assignment */
    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];
        struct drm_connector       *connector  = state->connectors[i];

        if (!conn_state || !connector) { continue; }

        if (conn_state->crtc_changed) {
            if (connector->state) { connector->state->crtc = conn_state->crtc; }
        }
    }

    drm_atomic_state_free(state);
    return 0;
}

/* ------------------------------------------------------------------ */
/* drm_atomic_nonblocking_commit: non-blocking atomic commit (MVP)     */
/* ------------------------------------------------------------------ */

int drm_atomic_nonblocking_commit(struct drm_atomic_state *state)
{
    /* MVP: same as synchronous commit */
    return drm_atomic_commit(state);
}