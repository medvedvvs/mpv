/*
 * This file is part of mpv.
 *
 * mpv is free software; you can redistribute it and/or
 * modify it under the terms of the GNU Lesser General Public
 * License as published by the Free Software Foundation; either
 * version 2.1 of the License, or (at your option) any later version.
 *
 * mpv is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU Lesser General Public License for more details.
 *
 * You should have received a copy of the GNU Lesser General Public
 * License along with mpv.  If not, see <http://www.gnu.org/licenses/>.
 */

#include <errno.h>
#include <inttypes.h>
#include <math.h>

#include "common/common.h"
#include "common/msg.h"
#include "drm_atomic.h"

int drm_object_create_properties(struct mp_log *log, int fd,
                                 struct drm_object *object)
{
    object->props = drmModeObjectGetProperties(fd, object->id, object->type);
    if (object->props) {
        object->props_info = talloc_zero_size(NULL, object->props->count_props
                                              * sizeof(object->props_info));
        if (object->props_info) {
            for (int i = 0; i < object->props->count_props; i++)
                object->props_info[i] = drmModeGetProperty(fd, object->props->props[i]);
        } else {
            mp_err(log, "Out of memory\n");
            goto fail;
        }
    } else {
        mp_err(log, "Failed to retrieve properties for object id %d\n", object->id);
        goto fail;
    }

    return 0;

  fail:
    drm_object_free_properties(object);
    return -1;
}

void drm_object_free_properties(struct drm_object *object)
{
    if (object->props) {
        for (int i = 0; i < object->props->count_props; i++) {
            if (object->props_info[i]) {
                drmModeFreeProperty(object->props_info[i]);
                object->props_info[i] = NULL;
            }
        }

        talloc_free(object->props_info);
        object->props_info = NULL;

        drmModeFreeObjectProperties(object->props);
        object->props = NULL;
    }
}

int drm_object_get_property(struct drm_object *object, char *name, uint64_t *value)
{
   for (int i = 0; i < object->props->count_props; i++) {
       if (strcasecmp(name, object->props_info[i]->name) == 0) {
           *value = object->props->prop_values[i];
           return 0;
       }
   }

   return -EINVAL;
}

drmModePropertyBlobPtr drm_object_get_property_blob(struct drm_object *object, char *name)
{
   uint64_t blob_id;

   if (!drm_object_get_property(object, name, &blob_id)) {
       return drmModeGetPropertyBlob(object->fd, blob_id);
   }

   return NULL;
}

int drm_object_set_property(drmModeAtomicReq *request, struct drm_object *object,
                            char *name, uint64_t value)
{
   for (int i = 0; i < object->props->count_props; i++) {
       if (strcasecmp(name, object->props_info[i]->name) == 0) {
           if (object->props_info[i]->flags & DRM_MODE_PROP_IMMUTABLE) {
               /* Do not try to set immutable values, as this might cause the
                * atomic commit operation to fail. */
               return -EINVAL;
           }
           return drmModeAtomicAddProperty(request, object->id,
                                           object->props_info[i]->prop_id, value);
       }
   }

   return -EINVAL;
}

struct drm_object * drm_object_create(struct mp_log *log, int fd,
                                      uint32_t object_id, uint32_t type)
{
    struct drm_object *obj = NULL;
    obj = talloc_zero(NULL, struct drm_object);
    obj->id = object_id;
    obj->type = type;
    obj->fd = fd;

    if (drm_object_create_properties(log, fd, obj)) {
        talloc_free(obj);
        return NULL;
    }

    return obj;
}

void drm_object_free(struct drm_object *object)
{
    if (object) {
        drm_object_free_properties(object);
        talloc_free(object);
    }
}

void drm_object_print_info(struct mp_log *log, struct drm_object *object)
{
    mp_err(log, "Object ID = %d (type = %x) has %d properties\n",
           object->id, object->type, object->props->count_props);

    for (int i = 0; i < object->props->count_props; i++)
        mp_err(log, "    Property '%s' = %lld\n", object->props_info[i]->name,
               (long long)object->props->prop_values[i]);
}

struct drm_atomic_context *drm_atomic_create_context(struct mp_log *log, int fd, int crtc_id,
                                                     int connector_id,
                                                     int draw_plane_idx, int drmprime_video_plane_idx)
{
    drmModePlaneRes *plane_res = NULL;
    drmModeRes *res = NULL;
    struct drm_object *plane = NULL;
    struct drm_atomic_context *ctx;
    int crtc_index = -1;
    int layercount = -1;
    int primary_id = 0;
    int overlay_id = 0;

    uint64_t value;

    res = drmModeGetResources(fd);
    if (!res) {
        mp_err(log, "Cannot retrieve DRM resources: %s\n", mp_strerror(errno));
        goto fail;
    }

    plane_res = drmModeGetPlaneResources(fd);
    if (!plane_res) {
        mp_err(log, "Cannot retrieve plane ressources: %s\n", mp_strerror(errno));
        goto fail;
    }

    ctx = talloc_zero(NULL, struct drm_atomic_context);
    if (!ctx) {
        mp_err(log, "Out of memory\n");
        goto fail;
    }

    ctx->fd = fd;
    ctx->crtc = drm_object_create(log, ctx->fd, crtc_id, DRM_MODE_OBJECT_CRTC);
    if (!ctx->crtc) {
        mp_err(log, "Failed to create CRTC object\n");
        goto fail;
    }

    for (int i = 0; i < res->count_crtcs; i++) {
        if (res->crtcs[i] == crtc_id) {
            crtc_index = i;
            break;
        }
    }

    for (int i = 0; i < res->count_connectors; i++) {
        drmModeConnector *connector = drmModeGetConnector(fd, res->connectors[i]);
        if (connector) {
            if (connector->connector_id == connector_id)
                ctx->connector =  drm_object_create(log, ctx->fd, connector->connector_id,
                                                    DRM_MODE_OBJECT_CONNECTOR);

            drmModeFreeConnector(connector);
            if (ctx->connector)
                break;
        }
    }

    for (unsigned int j = 0; j < plane_res->count_planes; j++) {

        drmModePlane *drmplane = drmModeGetPlane(ctx->fd, plane_res->planes[j]);
        const uint32_t possible_crtcs = drmplane->possible_crtcs;
        const uint32_t plane_id = drmplane->plane_id;
        drmModeFreePlane(drmplane);
        drmplane = NULL;

        if (possible_crtcs & (1 << crtc_index)) {
            plane = drm_object_create(log, ctx->fd, plane_id,
                                      DRM_MODE_OBJECT_PLANE);

            if (!plane) {
                mp_err(log, "Failed to create Plane object from plane ID %d\n",
                       plane_id);
                goto fail;
            }

            if (drm_object_get_property(plane, "TYPE", &value) == -EINVAL) {
                mp_err(log, "Unable to retrieve type property from plane %d\n", j);
                goto fail;
            }

            if (value != DRM_PLANE_TYPE_CURSOR) { // Skip cursor planes
                layercount++;

                if ((!primary_id) && (value == DRM_PLANE_TYPE_PRIMARY))
                    primary_id = plane_id;

                if ((!overlay_id) && (value == DRM_PLANE_TYPE_OVERLAY))
                    overlay_id = plane_id;

                if (layercount == draw_plane_idx) {
                    ctx->draw_plane = plane;
                    continue;
                }

                if (layercount == drmprime_video_plane_idx) {
                    ctx->drmprime_video_plane = plane;
                    continue;
                }
            }

            drm_object_free(plane);
            plane = NULL;
        }
    }

    // draw plane was specified as either of the special options: any primary plane or any overlay plane
    if (!ctx->draw_plane) {
        const int draw_plane_id = (draw_plane_idx == DRM_OPTS_OVERLAY_PLANE) ? overlay_id : primary_id;
        const char *plane_type = (draw_plane_idx == DRM_OPTS_OVERLAY_PLANE) ? "overlay" : "primary";
        if (draw_plane_id) {
            mp_verbose(log, "Using %s plane %d as draw plane\n", plane_type, draw_plane_id);
            ctx->draw_plane = drm_object_create(log, ctx->fd, draw_plane_id, DRM_MODE_OBJECT_PLANE);
        } else {
            mp_err(log, "Failed to find draw plane with idx=%d\n", draw_plane_idx);
            goto fail;
        }
    } else {
        mp_verbose(log, "Found draw plane with ID %d\n", ctx->draw_plane->id);
    }

    // drmprime plane was specified as either of the special options: any primary plane or any overlay plane
    if (!ctx->drmprime_video_plane) {
        const int drmprime_video_plane_id = (drmprime_video_plane_idx == DRM_OPTS_PRIMARY_PLANE) ? primary_id : overlay_id;
        const char *plane_type = (drmprime_video_plane_idx == DRM_OPTS_PRIMARY_PLANE) ? "primary" : "overlay";

        if (drmprime_video_plane_id) {
            mp_verbose(log, "Using %s plane %d as drmprime plane\n", plane_type, drmprime_video_plane_id);
            ctx->drmprime_video_plane = drm_object_create(log, ctx->fd, drmprime_video_plane_id, DRM_MODE_OBJECT_PLANE);
        } else {
            mp_verbose(log, "Failed to find drmprime plane with idx=%d. drmprime-drm hwdec interop will not work\n", drmprime_video_plane_idx);
        }
    } else {
        mp_verbose(log, "Found drmprime plane with ID %d\n", ctx->drmprime_video_plane->id);
    }

    drmModeFreePlaneResources(plane_res);
    drmModeFreeResources(res);
    return ctx;

fail:
    if (res)
        drmModeFreeResources(res);
    if (plane_res)
        drmModeFreePlaneResources(plane_res);
    if (plane)
        drm_object_free(plane);
    return NULL;
}

void drm_atomic_destroy_context(struct drm_atomic_context *ctx)
{
    drm_mode_destroy_blob(ctx->fd, &ctx->old_state.crtc.mode);
    drm_object_free(ctx->crtc);
    drm_object_free(ctx->connector);
    drm_object_free(ctx->draw_plane);
    drm_object_free(ctx->drmprime_video_plane);
    talloc_free(ctx);
}

static bool drm_atomic_save_plane_state(struct drm_object *plane,
                                        struct drm_atomic_plane_state *plane_state)
{
    if (!plane)
        return true;

    bool ret = true;

    if (0 > drm_object_get_property(plane, "FB_ID", &plane_state->fb_id))
        ret = false;
    if (0 > drm_object_get_property(plane, "CRTC_ID", &plane_state->crtc_id))
        ret = false;
    if (0 > drm_object_get_property(plane, "SRC_X", &plane_state->src_x))
        ret = false;
    if (0 > drm_object_get_property(plane, "SRC_Y", &plane_state->src_y))
        ret = false;
    if (0 > drm_object_get_property(plane, "SRC_W", &plane_state->src_w))
        ret = false;
    if (0 > drm_object_get_property(plane, "SRC_H", &plane_state->src_h))
        ret = false;
    if (0 > drm_object_get_property(plane, "CRTC_X", &plane_state->crtc_x))
        ret = false;
    if (0 > drm_object_get_property(plane, "CRTC_Y", &plane_state->crtc_y))
        ret = false;
    if (0 > drm_object_get_property(plane, "CRTC_W", &plane_state->crtc_w))
        ret = false;
    if (0 > drm_object_get_property(plane, "CRTC_H", &plane_state->crtc_h))
        ret = false;
    // ZPOS might not exist, so ignore whether or not this succeeds
    drm_object_get_property(plane, "ZPOS", &plane_state->zpos);

    return ret;
}

static bool drm_atomic_restore_plane_state(drmModeAtomicReq *request,
                                           struct drm_object *plane,
                                           const struct drm_atomic_plane_state *plane_state)
{
    if (!plane)
        return true;

    bool ret = true;

    if (0 > drm_object_set_property(request, plane, "FB_ID", plane_state->fb_id))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "CRTC_ID", plane_state->crtc_id))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "SRC_X", plane_state->src_x))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "SRC_Y", plane_state->src_y))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "SRC_W", plane_state->src_w))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "SRC_H", plane_state->src_h))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "CRTC_X", plane_state->crtc_x))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "CRTC_Y", plane_state->crtc_y))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "CRTC_W", plane_state->crtc_w))
        ret = false;
    if (0 > drm_object_set_property(request, plane, "CRTC_H", plane_state->crtc_h))
        ret = false;
    // ZPOS might not exist, or be immutable, so ignore whether or not this succeeds
    drm_object_set_property(request, plane, "ZPOS", plane_state->zpos);

    return ret;
}

bool drm_atomic_save_old_state(struct drm_atomic_context *ctx)
{
    if (ctx->old_state.saved)
        return false;

    bool ret = true;

    drmModeCrtc *crtc = drmModeGetCrtc(ctx->fd, ctx->crtc->id);
    if (crtc == NULL)
        return false;
    ctx->old_state.crtc.mode.mode = crtc->mode;
    drmModeFreeCrtc(crtc);

    if (0 > drm_object_get_property(ctx->crtc, "ACTIVE", &ctx->old_state.crtc.active))
        ret = false;

    if (0 > drm_object_get_property(ctx->connector, "CRTC_ID", &ctx->old_state.connector.crtc_id))
        ret = false;

    if (!drm_atomic_save_plane_state(ctx->draw_plane, &ctx->old_state.draw_plane))
        ret = false;
    if (!drm_atomic_save_plane_state(ctx->drmprime_video_plane, &ctx->old_state.drmprime_video_plane))
        ret = false;

    ctx->old_state.saved = true;

    return ret;
}

bool drm_atomic_restore_old_state(drmModeAtomicReqPtr request, struct drm_atomic_context *ctx)
{
    if (!ctx->old_state.saved)
        return false;

    bool ret = true;

    if (0 > drm_object_set_property(request, ctx->connector, "CRTC_ID", ctx->old_state.connector.crtc_id))
        ret = false;

    if (!drm_mode_ensure_blob(ctx->fd, &ctx->old_state.crtc.mode))
        ret = false;
    if (0 > drm_object_set_property(request, ctx->crtc, "MODE_ID", ctx->old_state.crtc.mode.blob_id))
        ret = false;
    if (0 > drm_object_set_property(request, ctx->crtc, "ACTIVE", ctx->old_state.crtc.active))
        ret = false;

    if (!drm_atomic_restore_plane_state(request, ctx->draw_plane, &ctx->old_state.draw_plane))
        ret = false;
    if (!drm_atomic_restore_plane_state(request, ctx->drmprime_video_plane, &ctx->old_state.drmprime_video_plane))
        ret = false;

    ctx->old_state.saved = false;

    return ret;
}

bool drm_mode_ensure_blob(int fd, struct drm_mode *mode)
{
    int ret = 0;

    if (!mode->blob_id) {
        ret = drmModeCreatePropertyBlob(fd, &mode->mode, sizeof(drmModeModeInfo),
                                        &mode->blob_id);
    }

    return (ret == 0);
}

bool drm_mode_destroy_blob(int fd, struct drm_mode *mode)
{
    int ret = 0;

    if (mode->blob_id) {
        ret = drmModeDestroyPropertyBlob(fd, mode->blob_id);
        mode->blob_id = 0;
    }

    return (ret == 0);
}

static uint32_t get_property_id(int fd, drmModeObjectProperties *props,
                const char *name)
{
    drmModePropertyPtr property;
    uint32_t i, id = 0;

    for (i = 0; i < props->count_props; i++) {
        property = drmModeGetProperty(fd, props->props[i]);
        if (!strcmp(property->name, name))
            id = property->prop_id;
        drmModeFreeProperty(property);

        if (id)
            break;
    }
    
    return id;
}


void drm_send_hdrmeta(struct drm_atomic_context *ctx, struct mp_colorspace *color) {
#ifdef DRM_HAS_HDR_METADATA_INFOFFRAME

    uint32_t property_hdr_id = get_property_id(ctx->connector->fd, ctx->connector->props, "HDR_OUTPUT_METADATA");
    if(!property_hdr_id)
        return;
    
    struct drm_hdr_metadata *p = &ctx->hdr_metadata;
    
    if(p->blob_id > 0)
        drmModeDestroyPropertyBlob(ctx->connector->fd, p->blob_id);
    p->data.metadata_type = DRM_HDMI_STATIC_METADATA_TYPE1; // = 1
    p->data.hdmi_metadata_type1.metadata_type = DRM_HDMI_STATIC_METADATA_TYPE1;
    struct mp_csp_primaries colors = mp_get_csp_primaries(color->primaries);

    /*		CTA‐861‐G says:
                    EOTF        EOTF of stream
                    0           Traditional gamma - SDR Luminance Range
                    1           Traditional gamma - HDR Luminance Range
                    2           SMPTE ST 2084 (mpv's PQ)
                    3            Hybrid Log-Gamma (HLG) based on ITU-R BT.2100-0 (mpv's HLG)
                    4- 7        Reserved for future use
    */
    // Note: bits below are set in the videodriver according to the number here (so, 3 -> 'set bit 3' == HLG)
    switch(color->gamma) {
        case MP_CSP_TRC_BT_1886: p->data.hdmi_metadata_type1.eotf = 1; break; // ???
        case MP_CSP_TRC_PQ: p->data.hdmi_metadata_type1.eotf = 2; break;
        case MP_CSP_TRC_HLG: p->data.hdmi_metadata_type1.eotf = 3; break;
        default: p->data.hdmi_metadata_type1.eotf = 0; break;		// default SDR
     }
     

    if(p->data.hdmi_metadata_type1.eotf) {

        float min_luminance = 0;	// ??? Is it correct?
        float max_luminance = color->sig_peak / 100.0; // [0..1]; mpv makes color management, so, all levels are the same?
        float max_cll = color->sig_peak / 100.0;
        float max_fall = color->sig_peak / 100.0;
        
        // Convert to  "16-bit values in units of 0.00002, where 0x0000 represents zero and 0xC350 represents 1.0000"
        p->data.hdmi_metadata_type1.display_primaries[0].x = round(colors.red.x * 50000.0);
        p->data.hdmi_metadata_type1.display_primaries[0].y = round(colors.red.y * 50000.0);
        p->data.hdmi_metadata_type1.display_primaries[1].x = round(colors.green.x * 50000.0);
        p->data.hdmi_metadata_type1.display_primaries[1].y = round(colors.green.y * 50000.0);
        p->data.hdmi_metadata_type1.display_primaries[2].x = round(colors.blue.x * 50000.0);
        p->data.hdmi_metadata_type1.display_primaries[2].y = round(colors.blue.y * 50000.0);
        // White point
        p->data.hdmi_metadata_type1.white_point.x = round(colors.white.x * 50000.0);
        p->data.hdmi_metadata_type1.white_point.y = round(colors.white.y * 50000.0);

        //... "16-bit value in units of 1 cd/m2, where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2"
        if(max_luminance >= 0)
            p->data.hdmi_metadata_type1.max_display_mastering_luminance = round(max_luminance * 65535);
        //... "16-bit value in units of 0.0001 cd/m2, where 0x0001 represents 0.0001 cd/m2 and 0xFFFF represents 6.5535 cd/m2"
        if(min_luminance >= 0)
            p->data.hdmi_metadata_type1.min_display_mastering_luminance = round(min_luminance * 65535);
        //... "16-bit value in units of 1 cd/m2, where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2."
        if (max_cll >= 0)
            p->data.hdmi_metadata_type1.max_cll = round(max_cll * 65535);
        //... "16-bit value in units of 1 cd/m2, where 0x0001 represents 1 cd/m2 and 0xFFFF represents 65535 cd/m2."
        if (max_fall >= 0)
            p->data.hdmi_metadata_type1.max_fall = round(max_fall * 65535);
       
       drmModeCreatePropertyBlob(ctx->connector->fd, &p->data, sizeof(p->data), &p->blob_id);
       drmModeAtomicReqPtr request = drmModeAtomicAlloc();
       if (!request)
            return;
       drmModeAtomicAddProperty(request, ctx->connector->id, property_hdr_id, p->blob_id);
       drmModeAtomicCommit(ctx->connector->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
//       printf("VVS: HDR status %d %d %d (HDR=%d b=%d) %.2f %.2f %.2f %.2f   %d %d %d\n", ret, ret1, ret2, property_hdr_id, p->blob_id, min_luminance, max_luminance, max_cll, max_fall, color->gamma, MP_CSP_TRC_PQ, MP_CSP_TRC_HLG);
       drmModeAtomicFree(request);
    }

#endif
}

// switch HDR off
void drm_destroy_hdrmeta(struct drm_atomic_context *ctx) {
#ifdef DRM_HAS_HDR_METADATA_INFOFFRAME
    if (ctx->hdr_metadata.blob_id) {
        drmModeDestroyPropertyBlob(ctx->connector->fd, ctx->hdr_metadata.blob_id);
        drmModeAtomicReqPtr request = drmModeAtomicAlloc();
        if (!request)
             return;
        drmModeAtomicCommit(ctx->connector->fd, request, DRM_MODE_ATOMIC_ALLOW_MODESET, NULL);
        drmModeAtomicFree(request);
        ctx->hdr_metadata.blob_id = 0;
     }
#endif
}
