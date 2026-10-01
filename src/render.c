#include "render.h"

#include <EGL/egl.h>
#include <EGL/eglext.h>
#include <GLES3/gl3.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <wayland-client.h>
#include <wayland-egl.h>

#include "config.h"
#include "shaders.h"

struct renderer {
    EGLDisplay dpy;
    EGLConfig config;
    EGLContext ctx;
    int verbose;

    bool program_ready;
    GLuint program;
    GLint u_time, u_amplitude, u_mode, u_resolution, u_frame;

    struct render_surface *current;
};

struct render_surface {
    struct renderer *r;
    struct wl_egl_window *egl_window;
    EGLSurface egl_surface;
    int width, height;
};

static const char *egl_error_string(void)
{
    switch (eglGetError()) {
    case EGL_SUCCESS: return "EGL_SUCCESS";
    case EGL_NOT_INITIALIZED: return "EGL_NOT_INITIALIZED";
    case EGL_BAD_ACCESS: return "EGL_BAD_ACCESS";
    case EGL_BAD_ALLOC: return "EGL_BAD_ALLOC";
    case EGL_BAD_ATTRIBUTE: return "EGL_BAD_ATTRIBUTE";
    case EGL_BAD_CONFIG: return "EGL_BAD_CONFIG";
    case EGL_BAD_CONTEXT: return "EGL_BAD_CONTEXT";
    case EGL_BAD_CURRENT_SURFACE: return "EGL_BAD_CURRENT_SURFACE";
    case EGL_BAD_DISPLAY: return "EGL_BAD_DISPLAY";
    case EGL_BAD_MATCH: return "EGL_BAD_MATCH";
    case EGL_BAD_NATIVE_PIXMAP: return "EGL_BAD_NATIVE_PIXMAP";
    case EGL_BAD_NATIVE_WINDOW: return "EGL_BAD_NATIVE_WINDOW";
    case EGL_BAD_PARAMETER: return "EGL_BAD_PARAMETER";
    case EGL_BAD_SURFACE: return "EGL_BAD_SURFACE";
    case EGL_CONTEXT_LOST: return "EGL_CONTEXT_LOST";
    default: return "unknown EGL error";
    }
}

static EGLDisplay get_display(struct wl_display *display)
{
    EGLDisplay dpy = EGL_NO_DISPLAY;

    /* EGL 1.5 core entry point. */
    dpy = eglGetPlatformDisplay(EGL_PLATFORM_WAYLAND_KHR, display, NULL);
    if (dpy != EGL_NO_DISPLAY)
        return dpy;

    /* Fall back to the EXT extension, then to the legacy entry point. */
    PFNEGLGETPLATFORMDISPLAYEXTPROC get_platform_display_ext =
        (PFNEGLGETPLATFORMDISPLAYEXTPROC)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (get_platform_display_ext) {
        dpy = get_platform_display_ext(EGL_PLATFORM_WAYLAND_EXT, display, NULL);
        if (dpy != EGL_NO_DISPLAY)
            return dpy;
    }
    return eglGetDisplay((EGLNativeDisplayType)display);
}

struct renderer *renderer_create(struct wl_display *display, int verbose)
{
    struct renderer *r = calloc(1, sizeof(*r));
    if (!r)
        return NULL;
    r->verbose = verbose;
    r->dpy = EGL_NO_DISPLAY;
    r->ctx = EGL_NO_CONTEXT;

    r->dpy = get_display(display);
    if (r->dpy == EGL_NO_DISPLAY) {
        fprintf(stderr, "soft-tempest: no EGL display for the Wayland connection (%s)\n",
                egl_error_string());
        goto fail;
    }

    EGLint major = 0, minor = 0;
    if (!eglInitialize(r->dpy, &major, &minor)) {
        fprintf(stderr, "soft-tempest: eglInitialize failed: %s\n", egl_error_string());
        r->dpy = EGL_NO_DISPLAY;
        goto fail;
    }
    if (verbose)
        fprintf(stderr, "soft-tempest: EGL %d.%d, vendor: %s\n", major, minor,
                eglQueryString(r->dpy, EGL_VENDOR));

    if (!eglBindAPI(EGL_OPENGL_ES_API)) {
        fprintf(stderr, "soft-tempest: eglBindAPI(EGL_OPENGL_ES_API) failed: %s\n",
                egl_error_string());
        goto fail;
    }

    static const EGLint config_attribs[] = {
        EGL_SURFACE_TYPE,    EGL_WINDOW_BIT,
        EGL_RENDERABLE_TYPE, EGL_OPENGL_ES3_BIT,
        EGL_RED_SIZE,        8,
        EGL_GREEN_SIZE,      8,
        EGL_BLUE_SIZE,       8,
        EGL_ALPHA_SIZE,      8, /* the overlay must be translucent */
        EGL_DEPTH_SIZE,      0,
        EGL_STENCIL_SIZE,    0,
        EGL_NONE
    };
    EGLint num_configs = 0;
    if (!eglChooseConfig(r->dpy, config_attribs, &r->config, 1, &num_configs) ||
        num_configs < 1) {
        fprintf(stderr, "soft-tempest: no EGL config with 8-bit alpha and GLES 3 (%s)\n",
                egl_error_string());
        goto fail;
    }

    static const EGLint ctx_attribs[] = {
        EGL_CONTEXT_MAJOR_VERSION, 3,
        EGL_CONTEXT_MINOR_VERSION, 0,
        EGL_NONE
    };
    r->ctx = eglCreateContext(r->dpy, r->config, EGL_NO_CONTEXT, ctx_attribs);
    if (r->ctx == EGL_NO_CONTEXT) {
        fprintf(stderr, "soft-tempest: cannot create an OpenGL ES 3.0 context: %s\n",
                egl_error_string());
        goto fail;
    }
    return r;

fail:
    renderer_destroy(r);
    return NULL;
}

void renderer_destroy(struct renderer *r)
{
    if (!r)
        return;
    if (r->dpy != EGL_NO_DISPLAY) {
        eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        if (r->program_ready && r->ctx != EGL_NO_CONTEXT) {
            /* Program objects die with the context; nothing else to free. */
        }
        if (r->ctx != EGL_NO_CONTEXT)
            eglDestroyContext(r->dpy, r->ctx);
        eglTerminate(r->dpy);
    }
    free(r);
}

static GLuint compile_shader(GLenum type, const char *src)
{
    GLuint sh = glCreateShader(type);
    glShaderSource(sh, 1, &src, NULL);
    glCompileShader(sh);
    GLint ok = GL_FALSE;
    glGetShaderiv(sh, GL_COMPILE_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        glGetShaderInfoLog(sh, sizeof(log), &len, log);
        fprintf(stderr, "soft-tempest: %s shader compilation failed:\n%.*s\n",
                type == GL_VERTEX_SHADER ? "vertex" : "fragment", (int)len, log);
        glDeleteShader(sh);
        return 0;
    }
    return sh;
}

static int build_program(struct renderer *r)
{
    GLuint vs = compile_shader(GL_VERTEX_SHADER, noise_vs);
    if (!vs)
        return -1;
    GLuint fs = compile_shader(GL_FRAGMENT_SHADER, noise_fs);
    if (!fs) {
        glDeleteShader(vs);
        return -1;
    }
    GLuint prog = glCreateProgram();
    glAttachShader(prog, vs);
    glAttachShader(prog, fs);
    glLinkProgram(prog);
    glDeleteShader(vs);
    glDeleteShader(fs);
    GLint ok = GL_FALSE;
    glGetProgramiv(prog, GL_LINK_STATUS, &ok);
    if (!ok) {
        char log[2048];
        GLsizei len = 0;
        glGetProgramInfoLog(prog, sizeof(log), &len, log);
        fprintf(stderr, "soft-tempest: shader program link failed:\n%.*s\n", (int)len, log);
        glDeleteProgram(prog);
        return -1;
    }
    r->program = prog;
    r->u_time = glGetUniformLocation(prog, "uTime");
    r->u_amplitude = glGetUniformLocation(prog, "uAmplitude");
    r->u_mode = glGetUniformLocation(prog, "uMode");
    r->u_resolution = glGetUniformLocation(prog, "uResolution");
    r->u_frame = glGetUniformLocation(prog, "uFrame");

    /* A vertex array object must be bound for glDrawArrays in GLES 3 on
     * some drivers even though no attributes are used. */
    GLuint vao = 0;
    glGenVertexArrays(1, &vao);
    glBindVertexArray(vao);

    glDisable(GL_DEPTH_TEST);
    glDisable(GL_SCISSOR_TEST);
    glDisable(GL_CULL_FACE);
    glEnable(GL_BLEND);
    /* Color: straight alpha over the transparent clear colour.
     * Alpha: ONE / ONE_MINUS_SRC_ALPHA so the stored alpha equals the source
     * alpha (plain SRC_ALPHA on the alpha channel would square it) and the
     * buffer ends up correctly premultiplied, as Wayland expects. */
    glBlendFuncSeparate(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA,
                        GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
    glClearColor(0.0f, 0.0f, 0.0f, 0.0f);

    if (r->verbose)
        fprintf(stderr, "soft-tempest: GL_RENDERER: %s, GL_VERSION: %s\n",
                (const char *)glGetString(GL_RENDERER),
                (const char *)glGetString(GL_VERSION));
    r->program_ready = true;
    return 0;
}

static int make_current(struct renderer *r, struct render_surface *rs)
{
    if (r->current == rs)
        return 0;
    if (!eglMakeCurrent(r->dpy, rs->egl_surface, rs->egl_surface, r->ctx)) {
        fprintf(stderr, "soft-tempest: eglMakeCurrent failed: %s\n", egl_error_string());
        r->current = NULL;
        return -1;
    }
    r->current = rs;
    /* We pace ourselves with wl_surface.frame callbacks; a non-zero swap
     * interval would make eglSwapBuffers block inside Mesa waiting for a
     * frame callback of its own, which does not mix with our poll() loop. */
    eglSwapInterval(r->dpy, 0);
    return 0;
}

struct render_surface *render_surface_create(struct renderer *r,
                                             struct wl_surface *surface,
                                             int width, int height)
{
    if (width <= 0 || height <= 0)
        return NULL;
    struct render_surface *rs = calloc(1, sizeof(*rs));
    if (!rs)
        return NULL;
    rs->r = r;
    rs->width = width;
    rs->height = height;
    rs->egl_surface = EGL_NO_SURFACE;

    rs->egl_window = wl_egl_window_create(surface, width, height);
    if (!rs->egl_window) {
        fprintf(stderr, "soft-tempest: wl_egl_window_create failed\n");
        free(rs);
        return NULL;
    }
    rs->egl_surface = eglCreatePlatformWindowSurface(r->dpy, r->config,
                                                     rs->egl_window, NULL);
    if (rs->egl_surface == EGL_NO_SURFACE) {
        /* Older EGL: the legacy entry point accepts the wl_egl_window too. */
        rs->egl_surface = eglCreateWindowSurface(
            r->dpy, r->config, (EGLNativeWindowType)rs->egl_window, NULL);
    }
    if (rs->egl_surface == EGL_NO_SURFACE) {
        fprintf(stderr, "soft-tempest: eglCreateWindowSurface failed: %s\n",
                egl_error_string());
        wl_egl_window_destroy(rs->egl_window);
        free(rs);
        return NULL;
    }
    return rs;
}

void render_surface_resize(struct render_surface *rs, int width, int height)
{
    if (!rs || width <= 0 || height <= 0)
        return;
    if (rs->width == width && rs->height == height)
        return;
    rs->width = width;
    rs->height = height;
    wl_egl_window_resize(rs->egl_window, width, height, 0, 0);
}

void render_surface_destroy(struct render_surface *rs)
{
    if (!rs)
        return;
    struct renderer *r = rs->r;
    if (r->current == rs) {
        eglMakeCurrent(r->dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
        r->current = NULL;
    }
    if (rs->egl_surface != EGL_NO_SURFACE)
        eglDestroySurface(r->dpy, rs->egl_surface);
    if (rs->egl_window)
        wl_egl_window_destroy(rs->egl_window);
    free(rs);
}

int render_frame(struct renderer *r, struct render_surface *rs,
                 const struct config *cfg, float time_s, uint32_t frame)
{
    if (make_current(r, rs) < 0)
        return -1;
    if (!r->program_ready && build_program(r) < 0)
        return -1;

    glViewport(0, 0, rs->width, rs->height);
    glClear(GL_COLOR_BUFFER_BIT);

    glUseProgram(r->program);
    glUniform1f(r->u_time, time_s);
    glUniform1f(r->u_amplitude, cfg->amplitude);
    glUniform1i(r->u_mode, cfg->mode);
    glUniform2f(r->u_resolution, (float)rs->width, (float)rs->height);
    glUniform1ui(r->u_frame, frame);
    glDrawArrays(GL_TRIANGLES, 0, 3);

    if (!eglSwapBuffers(r->dpy, rs->egl_surface)) {
        fprintf(stderr, "soft-tempest: eglSwapBuffers failed: %s\n", egl_error_string());
        return -1;
    }
    return 0;
}
