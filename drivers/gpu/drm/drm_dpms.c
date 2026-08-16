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

/* Return the userspace-visible name of a DPMS level ("Unknown" if out of range). */
const char *drm_get_dpms_name(int val)
{
    if (val < DRM_MODE_DPMS_ON || val > DRM_MODE_DPMS_OFF) return "Unknown";
    return drm_dpms_enum_list[val].name;
}

/* Create the standard legacy enum property "DPMS" and store it in mode_config.prop_dpms. */
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

/* Attach the standard DPMS property with an initial value of ON (connector construction time). */
int drm_connector_attach_dpms_property(struct drm_connector *connector)
{
    if (!connector || !connector->dev) return -EINVAL;

    return drm_object_attach_property(&connector->base, connector->dev->mode_config.prop_dpms, DRM_MODE_DPMS_ON);
}

/* Read the effective DPMS level; self-refresh forces ON (drm_atomic_connector_get_property). */
int drm_connector_dpms_get(struct drm_connector *connector)
{
    if (!connector) return DRM_MODE_DPMS_OFF;

    if (connector->state && connector->state->crtc && connector->state->crtc->state && connector->state->crtc->state->self_refresh_active) return DRM_MODE_DPMS_ON;

    return connector->dpms;
}

/* Atomically commit a DPMS level: clamp to ON/OFF, publish connector->dpms, recompute CRTC active, roll back on failure. */
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

    /* Deactivating the CRTC is an active-state change rejected without ALLOW_MODESET semantics. */
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

/* Legacy SETPROPERTY/OBJ_SETPROPERTY entry: atomic drivers commit, legacy drivers get the connector dpms hook. */
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