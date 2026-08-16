/*
 *
 *      drm_dpms.c
 *      DRM Display Power Management Signaling (DPMS) subsystem
 *
 *      2026/8/16 By JiTianYu391
 *      Copyright (C) 2020 ViudiraTech, based on the Apache 2.0 license.
 *
 */

#include <drivers/gpu/drm/drm_device.h>
#include <drivers/gpu/drm/drm_idr.h>
#include <drivers/gpu/drm/drm_mode.h>
#include <drivers/gpu/drm/drm_modeset_lock.h>
#include <drivers/gpu/drm/drm_print.h>
#include <kernel/errno.h>
#include <kernel/printk.h>
#include <libs/std/stdbool.h>
#include <libs/std/stddef.h>
#include <libs/std/stdint.h>
#include <libs/std/string.h>
#include <mem/alloc.h>
#include <sync/spin_lock.h>

/* Enum table for the "DPMS" property (names are part of the ABI). */
static const struct drm_mode_property_enum drm_dpms_enum_list[] = {
    {DRM_MODE_DPMS_ON,      "On"     },
    {DRM_MODE_DPMS_STANDBY, "Standby"},
    {DRM_MODE_DPMS_SUSPEND, "Suspend"},
    {DRM_MODE_DPMS_OFF,     "Off"    },
};

/*
 * drm_get_dpms_name - return the userspace-visible name of a DPMS level
 * @val: one of DRM_MODE_DPMS_*
 *
 * Returns: the matching string, or "Unknown" for out-of-range values.
 */
const char *drm_get_dpms_name(int val)
{
    if (val < DRM_MODE_DPMS_ON || val > DRM_MODE_DPMS_OFF) return "Unknown";
    return drm_dpms_enum_list[val].name;
}

/*
 * drm_mode_create_dpms_property - create the standard "DPMS" connector property
 * @dev: DRM device
 *
 * Creates the legacy enum property "DPMS" with the four standard levels and
 * stores it in dev->mode_config.prop_dpms. Does not fail if the property was
 * already created by a previous invocation.
 *
 * Returns: 0 on success or -ENOMEM on allocation failure.
 */
int drm_mode_create_dpms_property(struct drm_device *dev)
{
    struct drm_property *prop;

    if (!dev) return -EINVAL;

    if (dev->mode_config.prop_dpms) return 0;

    /* Matches Linux: plain enum property, no ATOMIC/IMMUTABLE bits. */
    prop = drm_property_create_enum(dev, 0, "DPMS", drm_dpms_enum_list, (int)(sizeof(drm_dpms_enum_list) / sizeof(drm_dpms_enum_list[0])));
    if (!prop) {
        plogk("drm_dpms: Failed to create DPMS property.\n");
        return -ENOMEM;
    }

    dev->mode_config.prop_dpms = prop;
    return 0;
}

/*
 * drm_connector_attach_dpms_property - attach the standard DPMS property
 * @connector: connector to attach to
 *
 * Attaches mode_config.prop_dpms with an initial value of DPMS_ON.
 * Must be called during connector construction, before the object is
 * published to userspace.
 *
 * Returns: 0 on success or a negative errno.
 */
int drm_connector_attach_dpms_property(struct drm_connector *connector)
{
    if (!connector || !connector->dev) return -EINVAL;

    return drm_object_attach_property(&connector->base, connector->dev->mode_config.prop_dpms, DRM_MODE_DPMS_ON);
}

/*
 * drm_connector_dpms_get - read the effective DPMS level of a connector
 * @connector: connector to query
 *
 * Follows drm_atomic_connector_get_property(): while the CRTC driving the
 * connector is in self-refresh, the output is still scanning out a stale
 * frame, so the reported level is forced back to ON.
 *
 * Returns: one of DRM_MODE_DPMS_*.
 */
int drm_connector_dpms_get(struct drm_connector *connector)
{
    if (!connector) return DRM_MODE_DPMS_OFF;

    if (connector->state && connector->state->crtc && connector->state->crtc->state && connector->state->crtc->state->self_refresh_active) return DRM_MODE_DPMS_ON;

    return connector->dpms;
}

/*
 * drm_connector_dpms_commit - atomically apply a DPMS level to a connector
 * @connector: connector to power-manage
 * @mode: one of DRM_MODE_DPMS_* (STANDBY/SUSPEND fold into OFF)
 *
 * Implements the atomic-driver DPMS path (drm_atomic_connector_commit_dpms):
 *   1. Clamp STANDBY/SUSPEND to OFF; a no-op ON/OFF change returns 0.
 *   2. Publish the new level in connector->dpms before committing so that
 *      concurrent legacy readers observe the pending transition.
 *   3. Deactivate the connector's CRTC unless another connector on the
 *      same CRTC is still ON.
 *   4. Roll connector->dpms back if the commit fails.
 *
 * Returns: 0 on success or a negative errno from the atomic commit.
 */
int drm_connector_dpms_commit(struct drm_connector *connector, int mode)
{
    struct drm_atomic_state *state;
    struct drm_crtc          *crtc;
    struct drm_crtc_state    *crtc_state;
    int                       ret;
    int                       old_mode;
    bool                      active = false;
    int                       i;

    if (!connector || !connector->dev) return -EINVAL;
    if (!(connector->dev->driver->driver_features & DRIVER_ATOMIC)) return -EOPNOTSUPP;

    if (mode != DRM_MODE_DPMS_ON) mode = DRM_MODE_DPMS_OFF;

    if (connector->dpms == mode) return 0;

    old_mode = connector->dpms;

    state = drm_atomic_state_alloc(connector->dev);
    if (!state) return -ENOMEM;

    /*
     * Deactivating the CRTC is an active-state change, which the check
     * phase rejects unless ALLOW_MODESET semantics are granted.  Legacy
     * DPMS in Linux implies a modeset-capable commit.
     */
    state->allow_modeset = true;

    /* Publish the pending level before the commit, like Linux. */
    connector->dpms = mode;

    crtc = connector->state ? connector->state->crtc : NULL;
    if (crtc) {
        ret = drm_atomic_add_affected_connectors(state, crtc);
        if (ret) goto out;

        crtc_state = drm_atomic_get_crtc_state(state, crtc);
        if (!crtc_state) {
            ret = -ENOMEM;
            goto out;
        }

        for (i = 0; i < state->num_connector; i++) {
            struct drm_connector *tmp_connector = state->connectors[i];
            if (!tmp_connector || !tmp_connector->state || tmp_connector->state->crtc != crtc) continue;
            if (tmp_connector->dpms == DRM_MODE_DPMS_ON) {
                active = true;
                break;
            }
        }

        crtc_state->active         = active;
        crtc_state->active_changed = !crtc->state || crtc->state->active != active;
    }

    ret = drm_atomic_commit(state);

out:
    if (ret != 0) connector->dpms = old_mode;

    drm_atomic_state_free(state);
    return ret;
}

/*
 * drm_connector_set_dpms - change the DPMS level of a connector
 * @connector: connector to power-manage
 * @mode: one of DRM_MODE_DPMS_*
 *
 * Entry point for the legacy SETPROPERTY / OBJ_SETPROPERTY ioctls, mirroring
 * Linux drm_connector_set_obj_prop(): the "DPMS" property is handled by the
 * core (never passed to generic property handlers):
 *   - atomic drivers go through drm_connector_dpms_commit();
 *   - legacy drivers get drm_connector_helper_funcs.dpms invoked with the
 *     raw 4-state value.
 * The "DPMS" property value in the per-object store is updated by the caller
 * on success, exactly like the legacy Linux path.
 *
 * Returns: 0 on success or a negative errno.
 */
int drm_connector_set_dpms(struct drm_connector *connector, int mode)
{
    struct drm_connector_helper_funcs *funcs;

    if (!connector) return -EINVAL;

    if (mode != DRM_MODE_DPMS_ON && mode != DRM_MODE_DPMS_STANDBY && mode != DRM_MODE_DPMS_SUSPEND && mode != DRM_MODE_DPMS_OFF) {
        plogk("drm_dpms: Connector %u invalid dpms value %d.\n", connector->base.id, mode);
        return -EINVAL;
    }

    if (connector->dev->driver->driver_features & DRIVER_ATOMIC) return drm_connector_dpms_commit(connector, mode);

    /* Legacy driver: delegate the 4-state control to the connector. */
    funcs = (struct drm_connector_helper_funcs *)connector->helper_private;
    if (!funcs || !funcs->dpms) {
        plogk("drm_dpms: Connector %u is legacy but has no dpms hook.\n", connector->base.id);
        return -EINVAL;
    }

    funcs->dpms(connector, mode);
    connector->dpms = mode;
    return 0;
}