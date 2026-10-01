/* One layer-shell overlay surface per wl_output. */
#ifndef SOFT_TEMPEST_OVERLAY_H
#define SOFT_TEMPEST_OVERLAY_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct app;
struct render_surface;

struct output {
    struct wl_list link;
    struct app *app;

    /* wl_output */
    struct wl_output *wl_output;
    uint32_t global_name;
    uint32_t version;
    char *name;        /* wl_output.name (v4), may be NULL */
    char *description; /* wl_output.description (v4), may be NULL */
    int32_t scale;     /* integer scale factor advertised by the output */
    int32_t mode_w, mode_h; /* current mode in physical pixels */
    bool done;         /* received at least one wl_output.done */
    bool skip_logged;  /* --output mismatch already reported */

    /* overlay surface */
    struct wl_surface *surface;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wp_viewport *viewport;
    struct wp_fractional_scale_v1 *fractional_scale;
    uint32_t scale120;     /* fractional scale * 120, 0 = not received yet */
    uint32_t logical_w, logical_h;
    uint32_t buffer_w, buffer_h;
    bool configured;

    /* rendering */
    struct render_surface *rs;
    struct wl_callback *frame_cb;
    bool frame_due;        /* compositor is ready for the next frame */
    int64_t next_frame_ns; /* earliest time the next frame may be drawn */
    uint32_t frame_counter;
    uint32_t frames_since_report; /* for the periodic --verbose statistics */
};

/* Called from the registry when a wl_output global appears / disappears. */
struct output *output_create(struct app *app, uint32_t global_name,
                             uint32_t version);
void output_destroy(struct output *out);

/* Create the overlay surface for an output if it is eligible and everything
 * it depends on (layer-shell, renderer) is available. Safe to call repeatedly. */
void overlay_maybe_create(struct output *out);
void overlay_destroy(struct output *out);

/* Render every output whose frame is due. Returns the monotonic time (ns) of
 * the earliest pending fps-cap deadline, or -1 if nothing is waiting. */
int64_t overlay_render_due(struct app *app, int64_t now_ns);

#endif
