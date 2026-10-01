# soft-tempest (Linux / Wayland)

A full-screen, click-through layer of **pixel noise re-randomised every frame**,
composited by the Wayland compositor in its *overlay* layer on every monitor.
It is a soft-TEMPEST display-masking tool in the sense of Kuhn & Anderson
(*Soft Tempest: Hidden Data Transmission Using Electromagnetic Emanations*, 1998):
the noise raises the noise floor of the electromagnetic emissions of the display
path, making the image on screen harder to reconstruct from a distance.

This is a **protective** tool for your own machine. It captures nothing, transmits
nothing and attacks nothing. It is the Linux/Wayland sibling of the Android and
Windows versions of the project (see `SPEC.md`).

## Threat model and known limitations — read this first

1. **Only compositors that implement `wlr-layer-shell` are supported.**
   The `zwlr_layer_shell_v1` protocol is implemented by wlroots-based compositors
   (Sway, Hyprland, river, Wayfire, labwc, niri, …) and by KWin (KDE Plasma).
   **GNOME / Mutter does not implement it, so soft-tempest cannot run on GNOME.**
   This is the main limitation of this port, the counterpart of `FLAG_SECURE`
   on Android or the Secure Desktop on Windows. When the protocol is missing the
   program prints a clear message and exits with status 2 instead of failing
   silently.
2. **Lock screens are not covered.** Session locks implemented with
   `ext-session-lock-v1` are drawn above the overlay layer. The noise is not
   drawn over them, and this is intentional. There is no workaround and none is
   attempted.
3. **Direct scanout.** A compositor can hand the buffer of a full-screen
   application straight to the display and skip composition. The presence of an
   overlay layer surface normally forces the compositor back into composition,
   so the noise is applied, but on unusual setups (multi-planes, overlay planes
   used for layer surfaces) this is worth verifying by looking at the screen.
4. **Averaging over N frames.** Static noise averages away as √N when an
   attacker integrates N frames. For that reason the noise **must** be
   independent in every frame; this program seeds a PCG hash in the fragment
   shader with the pixel coordinates and a per-frame counter. A naive static
   overlay would give an illusion of protection only.
5. **No measurement, no proof.** Nothing here has been measured with real
   TEMPEST receiving equipment. Treat it as a prototype and as a *complement*
   to hardware shielding, never as a replacement for it.
6. **GPU load and battery.** Rendering a full-screen surface every refresh
   costs GPU time and power. Use `--fps-cap` to trade some temporal
   decorrelation for a lower load.
7. **X11 is out of scope.** On an X11 session the equivalent would be an
   `override-redirect` window with an empty input shape. This repository is
   Wayland-only.

## How it works

For every `wl_output` the program creates one `wl_surface`, turns it into a
`zwlr_layer_surface_v1` in the `overlay` layer, anchors it to all four edges,
sets an exclusive zone of -1 (so it also covers panels and bars), disables
keyboard interactivity and gives the surface an **empty input region**, which
makes pointer and touch input fall straight through to whatever is underneath.

Rendering uses EGL on the Wayland platform (`EGL_PLATFORM_WAYLAND_KHR`), a
`wl_egl_window` and an OpenGL ES 3.0 context with an 8-bit alpha config. A
full-screen triangle generated from `gl_VertexID` is drawn with the fragment
shader in `src/shaders.h`; blending is `SRC_ALPHA, ONE_MINUS_SRC_ALPHA` for colour
over a transparent clear colour, with `ONE, ONE_MINUS_SRC_ALPHA` for alpha so the
buffer ends up correctly premultiplied. Frames are paced with `wl_surface.frame`
callbacks so they track the compositor's refresh rate; `--fps-cap` delays the
next draw after a callback when the configured rate is exceeded.

Output scale is handled so that noise is generated **per physical pixel**: when
the compositor offers `wp_fractional_scale_v1` and `wp_viewporter` the buffer is
`logical size × preferred scale` and a viewport maps it onto the logical size;
otherwise the integer `wl_output.scale` is used with `wl_surface.set_buffer_scale`.
Monitor hotplug is handled by watching the registry (new outputs get a surface,
removed outputs are torn down) and the layer surface `closed` event.

Noise modes (fragment shader uniform `uMode`):

| mode       | description                                                              |
|------------|--------------------------------------------------------------------------|
| `white`    | uniform white noise, independent per pixel and per frame                 |
| `hf`       | energy pushed into high horizontal frequencies (pixel clock): the sign alternates every column and is randomly modulated |
| `temporal` | like `white`, plus an extra random per-pixel, per-frame alpha modulation |

## Requirements

Build dependencies (Ubuntu / Debian package names):

```
libwayland-dev wayland-protocols libegl1-mesa-dev libgles2-mesa-dev \
meson ninja-build pkg-config
```

Which gives the pkg-config modules `wayland-client`, `wayland-egl`, `egl`,
`glesv2` plus the `wayland-scanner` program. `wlr-layer-shell-unstable-v1.xml`
is vendored in `protocol/` (it is not part of `wayland-protocols`). `xdg-shell`,
`viewporter` and `fractional-scale-v1` are taken from the system
`wayland-protocols` when version 1.31 or newer is installed, otherwise from the
vendored copies in `protocol/`.

Runtime: a Wayland compositor with `wlr-layer-shell` (see limitations) and a
working EGL/GLES 3.0 driver (Mesa is fine, including `llvmpipe`).

## Building

```
meson setup build --buildtype=release
meson compile -C build
./build/soft-tempest --help
```

`meson install -C build` installs the binary, a `.desktop` autostart entry and a
systemd user unit (see below).

## Usage

```
soft-tempest [options]

  -a, --amplitude <0..1>   noise opacity (default 0.25)
  -m, --mode <name>        white | hf | temporal (default white)
  -f, --fps-cap <N>        limit rendering to N frames/s (0 = compositor rate)
  -o, --output <name>      only cover the wl_output with this name (e.g. DP-1)
  -c, --config <path>      read settings from this file
      --no-config          ignore any config file
  -v, --verbose            log outputs, scales, sizes and fps to stderr
  -h, --help / -V, --version
```

The overlay starts immediately. `Ctrl+C` (SIGINT), SIGTERM or SIGHUP remove the
overlay and exit cleanly. Exit status: 0 on a clean shutdown, 1 on an error
(no Wayland display, EGL failure, lost connection), 2 when the compositor has no
`wlr-layer-shell`.

Output names are the ones shown by `swaymsg -t get_outputs`,
`hyprctl monitors` or `kscreen-doctor -o` (for example `DP-1`, `eDP-1`).

### Config file

Settings can also be stored in `$XDG_CONFIG_HOME/soft-tempest/config`
(`~/.config/soft-tempest/config` by default). Command-line options override
the file.

```
# ~/.config/soft-tempest/config
amplitude = 0.3
mode = hf
fps-cap = 60
# output = DP-1
```

### Autostart (optional, not part of the MVP)

Either copy `soft-tempest.desktop` to `~/.config/autostart/` (honoured by
compositors or session helpers that read XDG autostart, e.g. KDE Plasma), add
`exec soft-tempest` to your Sway config / `exec-once = soft-tempest` to
`hyprland.conf`, or use the systemd user unit:

```
systemctl --user enable --now soft-tempest.service
```

The unit is bound to `graphical-session.target` and does not restart on exit
status 2 (no layer-shell), so it will not loop on GNOME.

## Verifying it works

`--verbose` prints every output it covers, the logical and physical buffer
sizes, the GL renderer and, every 5 seconds, the achieved frame rate per output.

It can also be exercised without real hardware with a headless Sway:

```
export XDG_RUNTIME_DIR=/tmp/st-xdg; mkdir -p -m 700 $XDG_RUNTIME_DIR
WLR_BACKENDS=headless WLR_RENDERER=pixman WLR_LIBINPUT_NO_DEVICES=1 sway &
export WAYLAND_DISPLAY=wayland-1
./build/soft-tempest --verbose &
grim -t ppm shot1.ppm; grim -t ppm shot2.ppm   # two consecutive screenshots
swaymsg create_output                          # hotplug a second output
```

With a solid blue background and the default amplitude of 0.25, the red
channel of a screenshot is uniformly distributed over 0..64 (mean 32, standard
deviation 18.5), ~98% of the pixels differ between two consecutive screenshots
and the correlation between neighbouring pixels is zero, which is what per-pixel,
per-frame noise should look like.

## Continuous integration

`.github/workflows/build.yml` builds the project on `ubuntu-latest` with
`-Werror`, runs the parts that do not need a compositor (option parsing and the
"no Wayland display" path) and uploads `build/soft-tempest` as the
`soft-tempest-linux` artifact. The runner has no Wayland compositor, so
functional testing is done locally on Sway/Hyprland/KWin (or headless Sway as
above).

## Project layout

```
meson.build                    build definition + wayland-scanner code generation
protocol/                      protocol XML (wlr-layer-shell vendored; xdg-shell,
                               viewporter, fractional-scale as fallbacks)
src/main.c                     wl_display, registry, signals, poll() event loop, CLI
src/overlay.c/.h               one layer surface per output, input region, scale,
                               configure/ack, frame pacing, hotplug
src/render.c/.h                EGL display/context/surfaces, GLES program, uniforms
src/shaders.h                  GLSL ES 3.0 sources (noise_vs, noise_fs)
src/config.c/.h                command line and config file
src/app.h                      shared application state
soft-tempest.desktop           XDG autostart entry
soft-tempest.service           systemd user unit
.github/workflows/build.yml    CI build + artifact
SPEC.md                        the specification this implementation follows
```
