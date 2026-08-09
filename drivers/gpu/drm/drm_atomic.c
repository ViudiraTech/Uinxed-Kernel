/*
 *
 *      drm_atomic.c
 *      DRM atomic modesetting core
 *
 *      2026/7/22 By JiTianYu391
 *      Copyright © 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm_device.h>
#include <drivers/gpu/drm_idr.h>
#include <drivers/gpu/drm_mode.h>
#include <drivers/gpu/drm_modeset_lock.h>
#include <drivers/gpu/drm_print.h>
#include <kernel/errno.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <proc/task.h>
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

static void drm_atomic_state_default_clear(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     i;

    /* Free per-plane substates */
    if (state->planes) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct drm_plane_state *current = state->planes[i].state;
            struct drm_plane_state *old     = state->planes[i].old_state;
            struct drm_plane_state *new     = state->planes[i].new_state;
            free(current);
            if (old && old != current) free(old);
            if (new && new != current && new != old) free(new);
            state->planes[i].state = state->planes[i].old_state = state->planes[i].new_state = NULL;
        }
        free(state->planes);
        state->planes = NULL;
    }

    /* Free per-CRTC substates */
    if (state->crtcs) {
        for (i = 0; i < config->num_crtc; i++) {
            struct drm_crtc_state *current = state->crtcs[i].state;
            struct drm_crtc_state *old     = state->crtcs[i].old_state;
            struct drm_crtc_state *new     = state->crtcs[i].new_state;
            if (current && current->event) free(current->event);
            free(current);
            if (old && old != current) free(old);
            if (new && new != current && new != old) free(new);
            state->crtcs[i].state = state->crtcs[i].old_state = state->crtcs[i].new_state = NULL;
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

static void drm_atomic_state_clear(struct drm_atomic_state *state)
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
    if (crtc->state) {
        memcpy(crtc_entry->state, crtc->state, sizeof(*crtc_entry->state));
        /* Completion events are owned by the commit which allocated them;
         * cloning the pointer makes state teardown free an armed event. */
        crtc_entry->state->event = NULL;

        /* These bits describe changes made by one atomic transaction, not
         * properties of the committed CRTC.  Carrying them into the next
         * transaction makes an ordinary page flip look like a modeset and
         * causes non-ALLOW_MODESET commits to fail with -EINVAL. */
        crtc_entry->state->zpos_changed       = false;
        crtc_entry->state->mode_changed       = false;
        crtc_entry->state->active_changed     = false;
        crtc_entry->state->connectors_changed = false;
        crtc_entry->state->planes_changed     = false;
        crtc_entry->state->color_mgmt_changed = false;
    }

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
    if (plane->state) {
        memcpy(plane_entry->state, plane->state, sizeof(*plane_entry->state));
        plane_entry->state->zpos_changed = false;
    }

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

        new_connectors = malloc(sizeof(*new_connectors) * new_count); // NOLINT(bugprone-sizeof-expression)
        new_states     = malloc(sizeof(*new_states) * new_count); // NOLINT(bugprone-sizeof-expression)
        if (!new_connectors || !new_states) {
            free(new_connectors);
            free(new_states);
            return NULL;
        }
        if (state->num_connector) {
            memcpy(new_connectors, state->connectors, sizeof(*new_connectors) * state->num_connector);
            memcpy(new_states, state->connector_states, sizeof(*new_states) * state->num_connector);
        }
        free(state->connectors);
        free(state->connector_states);

        state->connectors                       = new_connectors;
        state->connector_states                 = new_states;
        state->connectors[state->num_connector] = connector;

        /* Allocate new connector state */
        state->connector_states[state->num_connector] = malloc(sizeof(*state->connector_states[0]));
        if (!state->connector_states[state->num_connector]) { return NULL; }
        memset(state->connector_states[state->num_connector], 0, sizeof(*state->connector_states[0]));
        state->connector_states[state->num_connector]->connector = connector;

        /* Copy from existing connector state if available */
        if (connector->state) {
            memcpy(state->connector_states[state->num_connector], connector->state, sizeof(*state->connector_states[0]));
            state->connector_states[state->num_connector]->link_status_changed = false;
            state->connector_states[state->num_connector]->crtc_changed        = false;
        }

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

        if (!state->allow_modeset && (crtc_state->mode_changed || crtc_state->active_changed)) return -EINVAL;

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

            if (!!plane_state->fb != !!plane_state->crtc) return -EINVAL;
            plane_state->visible = plane_state->fb && plane_state->crtc;
            if (plane_state->fb) {
                int64_t fb_w = (int64_t)plane_state->fb->width << 16;
                int64_t fb_h = (int64_t)plane_state->fb->height << 16;
                if (plane_state->src.x1 < 0 || plane_state->src.y1 < 0 || plane_state->src.x2 <= plane_state->src.x1
                    || plane_state->src.y2 <= plane_state->src.y1 || plane_state->src.x2 > fb_w || plane_state->src.y2 > fb_h
                    || plane_state->dst.x2 <= plane_state->dst.x1 || plane_state->dst.y2 <= plane_state->dst.y1)
                    return -EINVAL;
                if (!(plane_state->plane->possible_crtcs & (1U << plane_state->crtc->index))) return -EINVAL;
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

static int drm_atomic_commit_tail(struct drm_atomic_state *state)
{
    struct drm_device      *dev    = state->dev;
    struct drm_mode_config *config = &dev->mode_config;
    int                     ret;
    int                     i;

    ret = drm_atomic_check_only(state);
    if (ret < 0) { return ret; }

    /* Allocate all completion events before touching hardware. */
    if (state->page_flip_event) {
        int event_crtcs = 0;
        for (i = 0; i < config->num_crtc; i++) {
            struct drm_crtc_state *s = state->crtcs[i].state;
            if (!s) continue;
            s->event = malloc(sizeof(*s->event));
            if (!s->event) return -ENOMEM;
            memset(s->event, 0, sizeof(*s->event));
            s->event->dev               = dev;
            s->event->file_priv         = state->file_priv;
            s->event->crtc              = state->crtcs[i].ptr;
            s->event->pipe              = state->crtcs[i].ptr->index;
            s->event->event.base.type   = DRM_EVENT_FLIP_COMPLETE;
            s->event->event.base.length = sizeof(s->event->event);
            s->event->event.user_data   = state->user_data;
            s->event->event.crtc_id     = state->crtcs[i].ptr->base.id;
            event_crtcs++;
        }
        if (!event_crtcs) return -EINVAL;
    }

    /* Program scanout before publishing the new software state. */
    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc        *crtc          = state->crtcs[i].ptr;
        struct drm_crtc_state  *crtc_state    = state->crtcs[i].state;
        struct drm_plane_state *primary_state = NULL;
        if (!crtc || !crtc_state) continue;
        for (int p = 0; p < config->num_total_plane; p++) {
            if (state->planes[p].ptr == crtc->primary) {
                primary_state = state->planes[p].state;
                break;
            }
        }
        if (!primary_state && crtc->primary) primary_state = crtc->primary->state;
        if (crtc_state->active && primary_state && crtc_state->mode_changed) {
            struct drm_crtc_helper_funcs *h = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (!h || (!h->mode_set && !h->page_flip)) return -ENOSYS;
            if (h->mode_set)
                h->mode_set(crtc, primary_state->fb);
            else {
                ret = h->page_flip(crtc, primary_state->fb, NULL, 0);
                if (ret) return ret;
            }
        } else if (crtc_state->active && primary_state && (!crtc->primary->state || primary_state->fb != crtc->primary->state->fb)) {
            struct drm_crtc_helper_funcs *h = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (!h || !h->page_flip) return -ENOSYS;
            ret = h->page_flip(crtc, primary_state->fb, NULL, 0);
            if (ret) return ret;
        }
        if (!crtc_state->active && crtc->enabled && crtc->helper_private) {
            struct drm_crtc_helper_funcs *h = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (h->atomic_disable) h->atomic_disable(crtc, crtc->state);
        }
    }

    /* Apply CRTC states: enable/disable and set mode */
    for (i = 0; i < config->num_crtc; i++) {
        struct __drm_crtcs_state *crtc_entry = &state->crtcs[i];
        struct drm_crtc_state    *crtc_state = crtc_entry->state;

        if (!crtc_state || !crtc_entry->ptr) { continue; }

        if (crtc_state->active_changed) { crtc_entry->ptr->enabled = crtc_state->active; }

        if (crtc_state->mode_changed && crtc_state->active) { memcpy(&crtc_entry->ptr->mode, &crtc_state->mode, sizeof(crtc_state->mode)); }
        if (crtc_entry->ptr->state) {
            struct drm_pending_vblank_event *event = crtc_state->event;
            memcpy(crtc_entry->ptr->state, crtc_state, sizeof(*crtc_state));
            crtc_entry->ptr->state->event              = NULL;
            crtc_entry->ptr->state->zpos_changed       = false;
            crtc_entry->ptr->state->mode_changed       = false;
            crtc_entry->ptr->state->active_changed     = false;
            crtc_entry->ptr->state->connectors_changed = false;
            crtc_entry->ptr->state->planes_changed     = false;
            crtc_entry->ptr->state->color_mgmt_changed = false;
            crtc_state->event                          = event;
        }
    }

    /* Apply plane states: framebuffer and coordinates */
    if (state->planes) {
        for (i = 0; i < config->num_total_plane; i++) {
            struct __drm_planes_state *plane_entry = &state->planes[i];
            struct drm_plane_state    *plane_state = plane_entry->state;

            if (!plane_state || !plane_entry->ptr) { continue; }

            /* Apply framebuffer, including a complete plane disable. */
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
                plane_entry->ptr->state->zpos_changed     = false;
            }
            plane_entry->ptr->fb_id   = plane_state->fb ? plane_state->fb->base.id : 0;
            plane_entry->ptr->crtc_id = plane_state->crtc ? plane_state->crtc->base.id : 0;
        }
    }

    /* Apply connector states: CRTC assignment */
    for (i = 0; i < state->num_connector; i++) {
        struct drm_connector_state *conn_state = state->connector_states[i];
        struct drm_connector       *connector  = state->connectors[i];

        if (!conn_state || !connector) { continue; }

        if (connector->state) {
            if (conn_state->crtc_changed) { connector->state->crtc = conn_state->crtc; }
            connector->state->link_status_changed = false;
            connector->state->crtc_changed        = false;
        }
    }

    for (i = 0; i < config->num_crtc; i++) {
        struct drm_crtc       *crtc = state->crtcs[i].ptr;
        struct drm_crtc_state *s    = state->crtcs[i].state;
        if (!crtc || !s) continue;
        if (s->active && s->active_changed && crtc->helper_private) {
            struct drm_crtc_helper_funcs *h = (struct drm_crtc_helper_funcs *)crtc->helper_private;
            if (h->atomic_enable) h->atomic_enable(crtc, crtc->state);
        }
        if (s->event) {
            /* virtio-gpu waits for TRANSFER/SET_SCANOUT/FLUSH responses in
             * its page-flip callback.  For such a synchronous driver the
             * commit is already complete here; delaying the event until a
             * synthetic vblank leaves compositors stuck on their first
             * pending flip if no timer-driven vblank arrives. */
            if (dev->driver && (dev->driver->driver_features & DRIVER_SYNCHRONOUS_FLIP)) {
                s->event->sequence = drm_crtc_vblank_count(crtc);
                drm_crtc_send_vblank_event(crtc, s->event);
            } else if (crtc->enabled && drm_crtc_vblank_get(crtc) == 0) {
                s->event->vblank_ref = true;
                s->event->sequence   = (uint64_t)drm_crtc_vblank_count(crtc) + 1;
                drm_crtc_arm_vblank_event(crtc, s->event);
            } else {
                drm_crtc_send_vblank_event(crtc, s->event);
            }
            s->event = NULL;
        }
    }

    drm_atomic_state_free(state);
    return 0;
}

/* ------------------------------------------------------------------ */
/* Ordered blocking/nonblocking commit dispatch                         */
/* ------------------------------------------------------------------ */

static void drm_atomic_commit_wait_turn(struct drm_atomic_state *state)
{
    struct drm_mode_config *config = &state->dev->mode_config;

    spin_lock(&config->commit_queue_lock);
    if (!state->commit_seq) state->commit_seq = ++config->commit_queue_next;
    while (state->commit_seq != config->commit_queue_done + 1) {
        wait_queue_prepare(&config->commit_queue_wait);
        spin_unlock(&config->commit_queue_lock);
        wait_queue_sleep();
        spin_lock(&config->commit_queue_lock);
    }
    spin_unlock(&config->commit_queue_lock);
}

static void drm_atomic_commit_finish_turn(struct drm_device *dev)
{
    spin_lock(&dev->mode_config.commit_queue_lock);
    dev->mode_config.commit_queue_done++;
    spin_unlock(&dev->mode_config.commit_queue_lock);
    wait_queue_wake_all(&dev->mode_config.commit_queue_wait);
}

int drm_atomic_commit(struct drm_atomic_state *state)
{
    struct drm_device *dev;
    int                ret;

    if (!state || !state->dev) return -EINVAL;
    dev = state->dev;
    if (!state->commit_seq) {
        spin_lock(&dev->mode_config.commit_queue_lock);
        if (dev->mode_config.commit_queue_next != dev->mode_config.commit_queue_done) {
            spin_unlock(&dev->mode_config.commit_queue_lock);
            return -EBUSY;
        }
        state->commit_seq = ++dev->mode_config.commit_queue_next;
        spin_unlock(&dev->mode_config.commit_queue_lock);
    }
    drm_atomic_commit_wait_turn(state);
    ret = drm_atomic_commit_tail(state);
    drm_atomic_commit_finish_turn(dev);
    return ret;
}

static void drm_atomic_nonblock_worker(void *arg)
{
    struct drm_atomic_state *state     = (struct drm_atomic_state *)arg;
    struct drm_device       *dev       = state->dev;
    struct drm_file         *file_priv = state->file_priv;
    int                      ret       = drm_atomic_commit(state);

    if (ret) drm_atomic_state_free(state);
    if (file_priv) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_refs) file_priv->event_refs--;
        spin_unlock(&file_priv->event_lock);
        wait_queue_wake_all(&file_priv->event_wait);
    }
    drm_dev_put(dev);
}

int drm_atomic_nonblocking_commit(struct drm_atomic_state *state)
{
    struct drm_mode_config *config;
    struct drm_file        *file_priv;
    task_t                 *worker;

    if (!state || !state->dev) return -EINVAL;
    {
        int ret = drm_atomic_check_only(state);
        if (ret) return ret;
    }

    /* virtio-gpu's command path is synchronous already: it waits for every
     * TRANSFER/SET_SCANOUT/FLUSH response before page_flip returns.  Running
     * that work in another task adds no hardware concurrency and creates a
     * lost-wakeup window between the compositor's NONBLOCK ioctl and its
     * subsequent epoll_wait.  Complete it here so the flip event is queued
     * before the ioctl returns; epoll's initial level scan then observes it
     * even without depending on notification timing. */
    if (state->dev->driver && (state->dev->driver->driver_features & DRIVER_SYNCHRONOUS_FLIP)) {
        return drm_atomic_commit(state);
    }

    config    = &state->dev->mode_config;
    file_priv = state->file_priv;
    if (file_priv) {
        spin_lock(&file_priv->event_lock);
        if (file_priv->event_closing) {
            spin_unlock(&file_priv->event_lock);
            return -ENOENT;
        }
        file_priv->event_refs++;
        spin_unlock(&file_priv->event_lock);
    }
    if (!drm_dev_get(state->dev)) {
        if (file_priv) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
        }
        return -ENODEV;
    }

    spin_lock(&config->commit_queue_lock);
    if (config->commit_queue_next != config->commit_queue_done) {
        spin_unlock(&config->commit_queue_lock);
        drm_dev_put(state->dev);
        if (file_priv) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
            wait_queue_wake_all(&file_priv->event_wait);
        }
        return -EBUSY;
    }
    state->commit_seq = ++config->commit_queue_next;
    worker            = kthread_create("drm-atomic", drm_atomic_nonblock_worker, state);
    if (!worker) config->commit_queue_next--;
    spin_unlock(&config->commit_queue_lock);
    if (!worker) {
        drm_dev_put(state->dev);
        if (file_priv) {
            spin_lock(&file_priv->event_lock);
            file_priv->event_refs--;
            spin_unlock(&file_priv->event_lock);
            wait_queue_wake_all(&file_priv->event_wait);
        }
        return -ENOMEM;
    }
    return 0;
}
