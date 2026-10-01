/* Shared application state. */
#ifndef SOFT_TEMPEST_APP_H
#define SOFT_TEMPEST_APP_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

#include "config.h"

struct renderer;

struct app {
    struct config cfg;

    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct wp_viewporter *viewporter;
    struct wp_fractional_scale_manager_v1 *fractional_scale_manager;

    struct wl_list outputs; /* struct output::link */

    struct renderer *renderer;

    bool quit;
};

/* Monotonic clock in nanoseconds. */
int64_t app_now_ns(void);

/* Verbose logging (only when --verbose). */
void app_log(const struct app *app, const char *fmt, ...);

#endif
