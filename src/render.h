/* EGL + OpenGL ES 3.0 rendering of the noise overlay. */
#ifndef SOFT_TEMPEST_RENDER_H
#define SOFT_TEMPEST_RENDER_H

#include <stdint.h>

struct wl_display;
struct wl_surface;
struct config;
struct renderer;
struct render_surface;

/* Create the EGL display and a shared GLES 3.0 context. Returns NULL and
 * prints a message on failure. */
struct renderer *renderer_create(struct wl_display *display, int verbose);
void renderer_destroy(struct renderer *r);

/* Create / resize / destroy the EGL window surface backing one wl_surface.
 * Width and height are in physical buffer pixels. */
struct render_surface *render_surface_create(struct renderer *r,
                                             struct wl_surface *surface,
                                             int width, int height);
void render_surface_resize(struct render_surface *rs, int width, int height);
void render_surface_destroy(struct render_surface *rs);

/* Draw one frame of noise into the surface and swap (which commits the
 * wl_surface). Returns 0 on success, -1 if the frame could not be rendered. */
int render_frame(struct renderer *r, struct render_surface *rs,
                 const struct config *cfg, float time_s, uint32_t frame);

#endif
