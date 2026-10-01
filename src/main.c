/* soft-tempest: full-screen, click-through, per-frame noise overlay for
 * Wayland compositors implementing wlr-layer-shell.
 *
 * This file owns the Wayland connection, the registry, signal handling and
 * the poll() based event loop. Per-output surfaces live in overlay.c and the
 * EGL/GLES pipeline in render.c. */
#define _GNU_SOURCE
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "app.h"
#include "config.h"
#include "overlay.h"
#include "render.h"
#include "fractional-scale-v1-client-protocol.h"
#include "viewporter-client-protocol.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"

#define EXIT_NO_LAYER_SHELL 2

static int signal_pipe[2] = { -1, -1 };

static void signal_handler(int signo)
{
    unsigned char b = (unsigned char)signo;
    int saved = errno;
    if (write(signal_pipe[1], &b, 1) < 0) {
        /* nothing sensible to do in a signal handler */
    }
    errno = saved;
}

static int setup_signals(void)
{
    if (pipe(signal_pipe) < 0) {
        perror("soft-tempest: pipe");
        return -1;
    }
    for (int i = 0; i < 2; i++) {
        int flags = fcntl(signal_pipe[i], F_GETFL);
        fcntl(signal_pipe[i], F_SETFL, flags | O_NONBLOCK);
        fcntl(signal_pipe[i], F_SETFD, FD_CLOEXEC);
    }
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = SA_RESTART;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);
    sigaction(SIGHUP, &sa, NULL);
    signal(SIGPIPE, SIG_IGN);
    return 0;
}

int64_t app_now_ns(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000000000LL + ts.tv_nsec;
}

void app_log(const struct app *app, const char *fmt, ...)
{
    if (!app->cfg.verbose)
        return;
    va_list ap;
    va_start(ap, fmt);
    fputs("soft-tempest: ", stderr);
    vfprintf(stderr, fmt, ap);
    fputc('\n', stderr);
    va_end(ap);
}

/* ------------------------------------------------------------------------- */
/* registry                                                                  */
/* ------------------------------------------------------------------------- */

static uint32_t min_u32(uint32_t a, uint32_t b) { return a < b ? a : b; }

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t name, const char *interface,
                                   uint32_t version)
{
    struct app *app = data;

    if (!strcmp(interface, wl_compositor_interface.name)) {
        /* v3 for set_buffer_scale, v4 for set_input_region semantics we rely on. */
        app->compositor = wl_registry_bind(registry, name,
                                           &wl_compositor_interface,
                                           min_u32(version, 4));
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        app->layer_shell = wl_registry_bind(registry, name,
                                            &zwlr_layer_shell_v1_interface,
                                            min_u32(version, 4));
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        app->viewporter = wl_registry_bind(registry, name,
                                           &wp_viewporter_interface, 1);
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        app->fractional_scale_manager = wl_registry_bind(registry, name,
            &wp_fractional_scale_manager_v1_interface, 1);
    } else if (!strcmp(interface, wl_output_interface.name)) {
        if (!output_create(app, name, version))
            fprintf(stderr, "soft-tempest: failed to bind wl_output %u\n", name);
    }
}

static void registry_handle_global_remove(void *data,
                                          struct wl_registry *registry,
                                          uint32_t name)
{
    (void)registry;
    struct app *app = data;
    struct output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &app->outputs, link) {
        if (out->global_name == name) {
            app_log(app, "output %s: removed", out->name ? out->name : "?");
            output_destroy(out);
            return;
        }
    }
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

/* ------------------------------------------------------------------------- */
/* event loop                                                                */
/* ------------------------------------------------------------------------- */

static int run(struct app *app)
{
    struct pollfd fds[2];
    fds[0].fd = wl_display_get_fd(app->display);
    fds[0].events = POLLIN;
    fds[1].fd = signal_pipe[0];
    fds[1].events = POLLIN;

    const int64_t report_period = 5 * 1000000000LL;
    int64_t next_report = app_now_ns() + report_period;

    while (!app->quit) {
        /* Draw whatever is due; this queues commits that are flushed below. */
        int64_t now = app_now_ns();
        int64_t deadline = overlay_render_due(app, now);

        /* Standard libwayland multi-reader dance: dispatch everything that is
         * already queued, then flush and wait on the fd. */
        while (wl_display_prepare_read(app->display) != 0) {
            if (wl_display_dispatch_pending(app->display) < 0) {
                fprintf(stderr, "soft-tempest: wl_display_dispatch_pending: %s\n",
                        strerror(errno));
                return -1;
            }
        }
        /* Dispatching may have produced new frame requests. */
        now = app_now_ns();
        int64_t deadline2 = overlay_render_due(app, now);
        if (deadline2 >= 0 && (deadline < 0 || deadline2 < deadline))
            deadline = deadline2;

        if (wl_display_flush(app->display) < 0 && errno != EAGAIN) {
            fprintf(stderr, "soft-tempest: wl_display_flush: %s\n", strerror(errno));
            wl_display_cancel_read(app->display);
            return -1;
        }

        if (app->cfg.verbose) {
            now = app_now_ns();
            if (now >= next_report) {
                struct output *out;
                wl_list_for_each(out, &app->outputs, link) {
                    if (!out->surface)
                        continue;
                    app_log(app, "output %s: %.1f fps (%u frames total)",
                            out->name ? out->name : "?",
                            (double)out->frames_since_report * 1e9 /
                                (double)(now - next_report + report_period),
                            out->frame_counter);
                    out->frames_since_report = 0;
                }
                next_report = now + report_period;
            }
            if (deadline < 0 || next_report < deadline)
                deadline = next_report;
        }

        int timeout_ms = -1;
        if (deadline >= 0) {
            int64_t wait_ns = deadline - app_now_ns();
            if (wait_ns < 0)
                wait_ns = 0;
            timeout_ms = (int)((wait_ns + 999999) / 1000000);
        }

        int n = poll(fds, 2, timeout_ms);
        if (n < 0) {
            if (errno == EINTR) {
                wl_display_cancel_read(app->display);
                continue;
            }
            perror("soft-tempest: poll");
            wl_display_cancel_read(app->display);
            return -1;
        }

        if (fds[0].revents & POLLIN) {
            if (wl_display_read_events(app->display) < 0) {
                fprintf(stderr, "soft-tempest: lost connection to the compositor: %s\n",
                        strerror(errno));
                return -1;
            }
        } else {
            wl_display_cancel_read(app->display);
        }
        if (fds[0].revents & (POLLERR | POLLHUP)) {
            fprintf(stderr, "soft-tempest: compositor connection closed\n");
            return -1;
        }

        if (wl_display_dispatch_pending(app->display) < 0) {
            int err = wl_display_get_error(app->display);
            fprintf(stderr, "soft-tempest: protocol error: %s\n", strerror(err));
            return -1;
        }

        if (fds[1].revents & POLLIN) {
            unsigned char b;
            while (read(signal_pipe[0], &b, 1) == 1) {
                app_log(app, "received signal %d, shutting down", b);
                app->quit = true;
            }
        }
    }
    return 0;
}

static void app_cleanup(struct app *app)
{
    struct output *out, *tmp;
    wl_list_for_each_safe(out, tmp, &app->outputs, link)
        output_destroy(out);

    if (app->renderer)
        renderer_destroy(app->renderer);
    if (app->fractional_scale_manager)
        wp_fractional_scale_manager_v1_destroy(app->fractional_scale_manager);
    if (app->viewporter)
        wp_viewporter_destroy(app->viewporter);
    if (app->layer_shell)
        zwlr_layer_shell_v1_destroy(app->layer_shell);
    if (app->compositor)
        wl_compositor_destroy(app->compositor);
    if (app->registry)
        wl_registry_destroy(app->registry);
    if (app->display) {
        /* Make sure the surface destruction reaches the compositor so the
         * overlay disappears immediately. */
        wl_display_flush(app->display);
        wl_display_disconnect(app->display);
    }
    config_free(&app->cfg);
}

int main(int argc, char **argv)
{
    struct app app;
    memset(&app, 0, sizeof(app));
    wl_list_init(&app.outputs);
    config_defaults(&app.cfg);

    int r = config_load(&app.cfg, argc, argv);
    if (r != 0) {
        config_free(&app.cfg);
        return r > 0 ? EXIT_SUCCESS : EXIT_FAILURE;
    }

    app_log(&app, "amplitude=%.3f mode=%s fps-cap=%d output=%s",
            app.cfg.amplitude, config_mode_name(app.cfg.mode), app.cfg.fps_cap,
            app.cfg.output ? app.cfg.output : "(all)");

    if (setup_signals() < 0) {
        config_free(&app.cfg);
        return EXIT_FAILURE;
    }

    app.display = wl_display_connect(NULL);
    if (!app.display) {
        const char *wd = getenv("WAYLAND_DISPLAY");
        fprintf(stderr,
                "soft-tempest: cannot connect to a Wayland display (%s).\n"
                "  Is a Wayland compositor running and WAYLAND_DISPLAY set? "
                "X11 sessions are not supported.\n",
                wd ? wd : "WAYLAND_DISPLAY unset");
        config_free(&app.cfg);
        return EXIT_FAILURE;
    }

    app.registry = wl_display_get_registry(app.display);
    wl_registry_add_listener(app.registry, &registry_listener, &app);

    /* First round trip: all globals have been announced. */
    if (wl_display_roundtrip(app.display) < 0) {
        fprintf(stderr, "soft-tempest: wl_display_roundtrip failed\n");
        app_cleanup(&app);
        return EXIT_FAILURE;
    }

    if (!app.compositor) {
        fprintf(stderr, "soft-tempest: compositor does not advertise wl_compositor\n");
        app_cleanup(&app);
        return EXIT_FAILURE;
    }
    if (!app.layer_shell) {
        fprintf(stderr,
            "soft-tempest: this compositor does not support wlr-layer-shell "
            "(zwlr_layer_shell_v1).\n"
            "  The overlay needs a compositor that implements it: Sway, Hyprland, "
            "river, Wayfire, KWin (KDE Plasma) and other wlroots based compositors.\n"
            "  GNOME (Mutter) does not implement this protocol, so soft-tempest "
            "cannot run there.\n");
        app_cleanup(&app);
        return EXIT_NO_LAYER_SHELL;
    }
    if (!app.fractional_scale_manager || !app.viewporter)
        app_log(&app, "fractional-scale-v1/viewporter not available, "
                      "using integer output scale");

    app.renderer = renderer_create(app.display, app.cfg.verbose);
    if (!app.renderer) {
        app_cleanup(&app);
        return EXIT_FAILURE;
    }

    /* Second round trip: wl_output properties (name, scale, mode, done). */
    if (wl_display_roundtrip(app.display) < 0) {
        fprintf(stderr, "soft-tempest: wl_display_roundtrip failed\n");
        app_cleanup(&app);
        return EXIT_FAILURE;
    }

    if (wl_list_empty(&app.outputs)) {
        fprintf(stderr, "soft-tempest: the compositor has no outputs yet; "
                        "waiting for one to appear\n");
    }

    struct output *out;
    bool matched = false;
    wl_list_for_each(out, &app.outputs, link) {
        overlay_maybe_create(out);
        if (out->surface)
            matched = true;
    }
    if (app.cfg.output && !matched) {
        fprintf(stderr, "soft-tempest: no output named '%s' found (available:",
                app.cfg.output);
        wl_list_for_each(out, &app.outputs, link)
            fprintf(stderr, " %s", out->name ? out->name : "?");
        fprintf(stderr, "); waiting for it to be connected\n");
    }

    int ret = run(&app);
    app_log(&app, "exiting");
    app_cleanup(&app);
    return ret < 0 ? EXIT_FAILURE : EXIT_SUCCESS;
}
