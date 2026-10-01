#define _GNU_SOURCE
#include "overlay.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "app.h"
#include "render.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

static void overlay_request_frame(struct output *out);
static void overlay_render(struct output *out, int64_t now_ns);

/* ------------------------------------------------------------------------- */
/* wl_output                                                                 */
/* ------------------------------------------------------------------------- */

static void output_handle_geometry(void *data, struct wl_output *wl_output,
                                   int32_t x, int32_t y, int32_t physical_width,
                                   int32_t physical_height, int32_t subpixel,
                                   const char *make, const char *model,
                                   int32_t transform)
{
    (void)data; (void)wl_output; (void)x; (void)y; (void)physical_width;
    (void)physical_height; (void)subpixel; (void)make; (void)model; (void)transform;
}

static void output_handle_mode(void *data, struct wl_output *wl_output,
                               uint32_t flags, int32_t width, int32_t height,
                               int32_t refresh)
{
    (void)wl_output; (void)refresh;
    struct output *out = data;
    if (flags & WL_OUTPUT_MODE_CURRENT) {
        out->mode_w = width;
        out->mode_h = height;
    }
}

static void output_update_size(struct output *out);

static void output_handle_done(void *data, struct wl_output *wl_output)
{
    (void)wl_output;
    struct output *out = data;
    bool first = !out->done;
    out->done = true;
    if (first)
        app_log(out->app, "output %u: name=%s scale=%d mode=%dx%d (%s)",
                out->global_name, out->name ? out->name : "?", out->scale,
                out->mode_w, out->mode_h,
                out->description ? out->description : "");
    overlay_maybe_create(out);
    if (out->configured)
        output_update_size(out);
}

static void output_handle_scale(void *data, struct wl_output *wl_output,
                                int32_t factor)
{
    (void)wl_output;
    struct output *out = data;
    if (factor > 0)
        out->scale = factor;
}

static void output_handle_name(void *data, struct wl_output *wl_output,
                               const char *name)
{
    (void)wl_output;
    struct output *out = data;
    free(out->name);
    out->name = strdup(name);
}

static void output_handle_description(void *data, struct wl_output *wl_output,
                                      const char *description)
{
    (void)wl_output;
    struct output *out = data;
    free(out->description);
    out->description = strdup(description);
}

static const struct wl_output_listener output_listener = {
    .geometry = output_handle_geometry,
    .mode = output_handle_mode,
    .done = output_handle_done,
    .scale = output_handle_scale,
    .name = output_handle_name,
    .description = output_handle_description,
};

struct output *output_create(struct app *app, uint32_t global_name,
                             uint32_t version)
{
    struct output *out = calloc(1, sizeof(*out));
    if (!out)
        return NULL;
    out->app = app;
    out->global_name = global_name;
    out->scale = 1;
    /* v4 brings name/description; v3 brings release(); v2 scale/done. */
    out->version = version < 4 ? version : 4;
    out->wl_output = wl_registry_bind(app->registry, global_name,
                                      &wl_output_interface, out->version);
    if (!out->wl_output) {
        free(out);
        return NULL;
    }
    wl_output_add_listener(out->wl_output, &output_listener, out);
    wl_list_insert(&app->outputs, &out->link);
    return out;
}

void output_destroy(struct output *out)
{
    if (!out)
        return;
    overlay_destroy(out);
    wl_list_remove(&out->link);
    if (out->wl_output) {
        if (out->version >= 3)
            wl_output_release(out->wl_output);
        else
            wl_output_destroy(out->wl_output);
    }
    free(out->name);
    free(out->description);
    free(out);
}

/* ------------------------------------------------------------------------- */
/* Buffer size / scale handling                                              */
/* ------------------------------------------------------------------------- */

static void compute_buffer_size(struct output *out)
{
    uint32_t lw = out->logical_w, lh = out->logical_h;
    if (lw == 0 || lh == 0) {
        /* Should not happen with all four anchors set, but be defensive and
         * fall back to the output mode. */
        lw = (uint32_t)out->mode_w / (uint32_t)(out->scale > 0 ? out->scale : 1);
        lh = (uint32_t)out->mode_h / (uint32_t)(out->scale > 0 ? out->scale : 1);
    }
    if (out->fractional_scale && out->scale120 > 0) {
        /* Round to nearest, as fractional-scale-v1 recommends. */
        out->buffer_w = (lw * out->scale120 + 60) / 120;
        out->buffer_h = (lh * out->scale120 + 60) / 120;
    } else {
        out->buffer_w = lw * (uint32_t)out->scale;
        out->buffer_h = lh * (uint32_t)out->scale;
    }
    if (out->buffer_w == 0) out->buffer_w = 1;
    if (out->buffer_h == 0) out->buffer_h = 1;
}

/* Apply the current logical size and scale to the surface state and the EGL
 * window. Must be followed by a commit (eglSwapBuffers does one). */
static void output_update_size(struct output *out)
{
    if (!out->surface || !out->configured)
        return;
    uint32_t old_w = out->buffer_w, old_h = out->buffer_h;
    compute_buffer_size(out);

    if (out->viewport) {
        /* With a viewport the buffer can be any size; the compositor scales it
         * to the logical surface size. Buffer scale stays at 1. */
        wp_viewport_set_destination(out->viewport, (int32_t)out->logical_w,
                                    (int32_t)out->logical_h);
    } else {
        wl_surface_set_buffer_scale(out->surface, out->scale);
    }

    if (!out->rs) {
        out->rs = render_surface_create(out->app->renderer, out->surface,
                                        (int)out->buffer_w, (int)out->buffer_h);
        if (!out->rs) {
            fprintf(stderr, "soft-tempest: cannot create EGL surface for output %s\n",
                    out->name ? out->name : "?");
            return;
        }
        app_log(out->app, "output %s: overlay %ux%u logical, %ux%u buffer pixels%s",
                out->name ? out->name : "?", out->logical_w, out->logical_h,
                out->buffer_w, out->buffer_h,
                out->viewport ? " (fractional-scale)" : "");
    } else if (old_w != out->buffer_w || old_h != out->buffer_h) {
        render_surface_resize(out->rs, (int)out->buffer_w, (int)out->buffer_h);
        app_log(out->app, "output %s: resized to %ux%u buffer pixels",
                out->name ? out->name : "?", out->buffer_w, out->buffer_h);
    }
}

/* ------------------------------------------------------------------------- */
/* fractional scale                                                          */
/* ------------------------------------------------------------------------- */

static void fractional_scale_handle_preferred(void *data,
        struct wp_fractional_scale_v1 *fractional_scale, uint32_t scale)
{
    (void)fractional_scale;
    struct output *out = data;
    if (out->scale120 == scale)
        return;
    out->scale120 = scale;
    app_log(out->app, "output %s: preferred fractional scale %u/120",
            out->name ? out->name : "?", scale);
    if (out->configured) {
        output_update_size(out);
        /* Push a frame at the new size as soon as the compositor allows. */
        if (!out->frame_cb)
            out->frame_due = true;
    }
}

static const struct wp_fractional_scale_v1_listener fractional_scale_listener = {
    .preferred_scale = fractional_scale_handle_preferred,
};

/* ------------------------------------------------------------------------- */
/* layer surface                                                             */
/* ------------------------------------------------------------------------- */

static void layer_surface_handle_configure(void *data,
        struct zwlr_layer_surface_v1 *layer_surface, uint32_t serial,
        uint32_t width, uint32_t height)
{
    struct output *out = data;
    zwlr_layer_surface_v1_ack_configure(layer_surface, serial);

    bool first = !out->configured;
    out->configured = true;
    out->logical_w = width;
    out->logical_h = height;
    output_update_size(out);

    /* The ack must be followed by a commit with a buffer; drawing a frame
     * right away does exactly that (eglSwapBuffers commits). Later
     * reconfigures just mark the surface due so the fps cap still applies. */
    if (first)
        overlay_render(out, app_now_ns());
    else if (!out->frame_cb)
        out->frame_due = true;
}

static void layer_surface_handle_closed(void *data,
        struct zwlr_layer_surface_v1 *layer_surface)
{
    (void)layer_surface;
    struct output *out = data;
    app_log(out->app, "output %s: layer surface closed by the compositor",
            out->name ? out->name : "?");
    overlay_destroy(out);
}

static const struct zwlr_layer_surface_v1_listener layer_surface_listener = {
    .configure = layer_surface_handle_configure,
    .closed = layer_surface_handle_closed,
};

static bool output_eligible(const struct output *out)
{
    const char *want = out->app->cfg.output;
    if (!want)
        return true;
    if (out->name && !strcmp(out->name, want))
        return true;
    return false;
}

void overlay_maybe_create(struct output *out)
{
    struct app *app = out->app;
    if (out->surface || !out->done)
        return;
    if (!app->layer_shell || !app->compositor || !app->renderer)
        return;
    if (!output_eligible(out)) {
        if (!out->skip_logged)
            app_log(app, "output %s: skipped (--output %s)",
                    out->name ? out->name : "?", app->cfg.output);
        out->skip_logged = true;
        return;
    }

    out->surface = wl_compositor_create_surface(app->compositor);
    if (!out->surface)
        return;

    /* Click-through: an empty input region means every pointer/touch event
     * falls through to whatever is underneath. */
    struct wl_region *empty = wl_compositor_create_region(app->compositor);
    wl_surface_set_input_region(out->surface, empty);
    wl_region_destroy(empty);

    if (app->fractional_scale_manager && app->viewporter) {
        out->fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(
            app->fractional_scale_manager, out->surface);
        wp_fractional_scale_v1_add_listener(out->fractional_scale,
                                            &fractional_scale_listener, out);
        out->viewport = wp_viewporter_get_viewport(app->viewporter, out->surface);
    }

    out->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        app->layer_shell, out->surface, out->wl_output,
        ZWLR_LAYER_SHELL_V1_LAYER_OVERLAY, "soft-tempest");
    zwlr_layer_surface_v1_add_listener(out->layer_surface,
                                       &layer_surface_listener, out);

    zwlr_layer_surface_v1_set_anchor(out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP | ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    /* -1: ignore other surfaces' exclusive zones and cover panels/bars too. */
    zwlr_layer_surface_v1_set_exclusive_zone(out->layer_surface, -1);
    zwlr_layer_surface_v1_set_keyboard_interactivity(out->layer_surface,
        ZWLR_LAYER_SURFACE_V1_KEYBOARD_INTERACTIVITY_NONE);
    /* Size 0x0 with all anchors set: the compositor picks the output size. */
    zwlr_layer_surface_v1_set_size(out->layer_surface, 0, 0);

    /* Initial commit without a buffer; the compositor answers with configure. */
    wl_surface_commit(out->surface);
    app_log(app, "output %s: layer surface created, waiting for configure",
            out->name ? out->name : "?");
}

void overlay_destroy(struct output *out)
{
    if (out->frame_cb) {
        wl_callback_destroy(out->frame_cb);
        out->frame_cb = NULL;
    }
    if (out->rs) {
        render_surface_destroy(out->rs);
        out->rs = NULL;
    }
    if (out->fractional_scale) {
        wp_fractional_scale_v1_destroy(out->fractional_scale);
        out->fractional_scale = NULL;
    }
    if (out->viewport) {
        wp_viewport_destroy(out->viewport);
        out->viewport = NULL;
    }
    if (out->layer_surface) {
        zwlr_layer_surface_v1_destroy(out->layer_surface);
        out->layer_surface = NULL;
    }
    if (out->surface) {
        wl_surface_destroy(out->surface);
        out->surface = NULL;
    }
    out->configured = false;
    out->frame_due = false;
    out->scale120 = 0;
    out->buffer_w = out->buffer_h = 0;
}

/* ------------------------------------------------------------------------- */
/* frame pacing                                                              */
/* ------------------------------------------------------------------------- */

static void frame_handle_done(void *data, struct wl_callback *cb, uint32_t time)
{
    (void)time;
    struct output *out = data;
    wl_callback_destroy(cb);
    out->frame_cb = NULL;
    /* The actual draw happens in overlay_render_due() from the main loop so
     * that the fps cap can delay it. */
    out->frame_due = true;
}

static const struct wl_callback_listener frame_listener = {
    .done = frame_handle_done,
};

static void overlay_request_frame(struct output *out)
{
    if (out->frame_cb)
        return;
    out->frame_cb = wl_surface_frame(out->surface);
    wl_callback_add_listener(out->frame_cb, &frame_listener, out);
}

static void overlay_render(struct output *out, int64_t now_ns)
{
    struct app *app = out->app;
    out->frame_due = false;
    if (!out->rs)
        return;

    /* Request the next frame callback before swapping so that it rides on the
     * commit performed by eglSwapBuffers. */
    overlay_request_frame(out);

    float t = (float)((double)now_ns / 1e9);
    out->frame_counter++;
    out->frames_since_report++;
    if (render_frame(app->renderer, out->rs, &app->cfg, t, out->frame_counter) < 0) {
        /* Rendering is broken for this surface; stop asking for frames so we
         * do not spin, but keep the (transparent) surface alive. */
        if (out->frame_cb) {
            wl_callback_destroy(out->frame_cb);
            out->frame_cb = NULL;
        }
        return;
    }

    if (app->cfg.fps_cap > 0) {
        int64_t period = 1000000000LL / app->cfg.fps_cap;
        /* Keep the cadence stable rather than drifting by the render time,
         * but never accumulate a backlog. */
        int64_t next = out->next_frame_ns + period;
        if (next < now_ns)
            next = now_ns + period;
        out->next_frame_ns = next;
    }
}

int64_t overlay_render_due(struct app *app, int64_t now_ns)
{
    int64_t earliest = -1;
    struct output *out;
    wl_list_for_each(out, &app->outputs, link) {
        if (!out->frame_due || !out->configured)
            continue;
        if (app->cfg.fps_cap > 0 && out->next_frame_ns > now_ns) {
            if (earliest < 0 || out->next_frame_ns < earliest)
                earliest = out->next_frame_ns;
            continue;
        }
        overlay_render(out, now_ns);
    }
    return earliest;
}
